/**
 * @file llviewermcp.cpp
 * @brief Loopback MCP server for the running viewer session.
 *
 * Listens on 127.0.0.1 when EnableViewerMCP is set. Tool calls run on the
 * main thread. This controls the logged-in viewer, not a second avatar.
 */

#include "llviewerprecompiledheaders.h"

#include "llviewermcp.h"

#include "llassetstorage.h"
#include "llfilesystem.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llagentdata.h"
#include "llagentui.h"
#include "llappearancemgr.h"
#include "llcommandhandler.h"
#include "lldir.h"
#include "llhudeffectlookat.h"
#include "llimview.h"
#include "llinventorybridge.h"
#include "llinventorymodel.h"
#include "llnamevalue.h"
#include "llpermissions.h"
#include "llprimitive.h"
#include "roles_constants.h"
#include "llselectmgr.h"
#include "llstartup.h"
#include "lltooldraganddrop.h"
#include "lltoolgrab.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewernetwork.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llvolumemessage.h"
#include "material_codes.h"
#include "object_flags.h"

#include "boost/json.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#if !LL_WINDOWS
#include <fcntl.h>
#include <sys/stat.h>
#endif

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#if LL_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
using mcp_socket_t = SOCKET;
static const mcp_socket_t MCP_INVALID_SOCKET = INVALID_SOCKET;
static void mcp_close_socket(mcp_socket_t s) { if (s != INVALID_SOCKET) closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using mcp_socket_t = int;
static const mcp_socket_t MCP_INVALID_SOCKET = -1;
static void mcp_close_socket(mcp_socket_t s) { if (s >= 0) ::close(s); }
#endif

extern void send_chat_from_viewer(const std::string& utf8_out_text, EChatType type, S32 channel);

namespace
{
    constexpr S32 kMaxChat = 100;
    constexpr S32 kMaxNearby = 100;
    constexpr F64 kChatThrottleSeconds = 1.0;

    struct ChatLine
    {
        std::string kind;
        std::string from;
        LLUUID from_id;
        std::string text;
    };

    struct Job
    {
        std::function<void(boost::json::object& out, bool& is_error)> fn;
        boost::json::object result;
        bool is_error = false;
        bool done = false;
        std::mutex mu;
        std::condition_variable cv;
    };

    std::mutex gQueueMu;
    std::deque<std::shared_ptr<Job>> gQueue;

    std::mutex gChatMu;
    std::deque<ChatLine> gChat;

    std::atomic<bool> gRun{false};
    std::atomic<bool> gShutdown{false};
    std::thread gThread;
    mcp_socket_t gListen = MCP_INVALID_SOCKET;
    int gPort = 9411;
    F64 gLastChatTime = 0.0;
    bool gStarted = false;

    bool in_world()
    {
        return LLStartUp::getStartupState() == STATE_STARTED && isAgentAvatarValid();
    }

    void require_world(bool& is_error, const std::string& what)
    {
        if (!in_world())
        {
            is_error = true;
            throw std::runtime_error(what + " requires the avatar to be in world");
        }
    }

    std::string object_label(LLViewerObject* obj)
    {
        if (!obj)
        {
            return std::string();
        }
        if (obj->isAvatar())
        {
            return static_cast<LLVOAvatar*>(obj)->getFullname();
        }
        LLNameValue* nv = obj->getNVPair("Name");
        if (nv && nv->getString())
        {
            return nv->getString();
        }
        return std::string();
    }

    boost::json::array link_ids(LLViewerObject* obj)
    {
        boost::json::array ids;
        if (!obj || obj->isAvatar())
        {
            return ids;
        }
        LLViewerObject* root = obj->getRootEdit();
        if (!root || root->isAvatar())
        {
            return ids;
        }
        std::vector<LLViewerObject*> links;
        root->addThisAndNonJointChildren(links);
        for (LLViewerObject* link : links)
        {
            if (link)
            {
                ids.push_back(boost::json::string(link->getID().asString()));
            }
        }
        return ids;
    }

    LLViewerObject* object_by_id(const std::string& id_text, bool& is_error)
    {
        LLUUID id(id_text);
        if (id.isNull())
        {
            is_error = true;
            throw std::runtime_error("object id is not a UUID");
        }
        LLViewerObject* obj = gObjectList.findObject(id);
        if (!obj)
        {
            is_error = true;
            throw std::runtime_error("object is not in the viewer cache");
        }
        return obj;
    }

    const boost::json::object* args_of(const boost::json::object& params)
    {
        auto it = params.find("arguments");
        if (it == params.end() || !it->value().is_object())
        {
            return nullptr;
        }
        return &it->value().as_object();
    }

    std::string jstr(const boost::json::object* args, const char* key, const std::string& fallback = std::string())
    {
        if (!args)
        {
            return fallback;
        }
        auto it = args->find(key);
        if (it == args->end() || !it->value().is_string())
        {
            return fallback;
        }
        return std::string(it->value().as_string());
    }

    double jreal(const boost::json::object* args, const char* key, double fallback)
    {
        if (!args)
        {
            return fallback;
        }
        auto it = args->find(key);
        if (it == args->end())
        {
            return fallback;
        }
        if (it->value().is_double())
        {
            return it->value().as_double();
        }
        if (it->value().is_int64())
        {
            return static_cast<double>(it->value().as_int64());
        }
        if (it->value().is_uint64())
        {
            return static_cast<double>(it->value().as_uint64());
        }
        return fallback;
    }

    bool jhas(const boost::json::object* args, const char* key)
    {
        return args && args->find(key) != args->end();
    }

    bool jbool(const boost::json::object* args, const char* key, bool fallback)
    {
        if (!args)
        {
            return fallback;
        }
        auto it = args->find(key);
        if (it == args->end() || !it->value().is_bool())
        {
            return fallback;
        }
        return it->value().as_bool();
    }

    boost::json::object vec3_json(const LLVector3& v)
    {
        boost::json::object o;
        o["x"] = v.mV[VX];
        o["y"] = v.mV[VY];
        o["z"] = v.mV[VZ];
        return o;
    }

    std::string camera_mode_name()
    {
        switch (gAgentCamera.getCameraMode())
        {
        case CAMERA_MODE_THIRD_PERSON: return "third_person";
        case CAMERA_MODE_MOUSELOOK: return "mouselook";
        case CAMERA_MODE_CUSTOMIZE_AVATAR: return "customize_avatar";
        case CAMERA_MODE_FOLLOW: return "follow";
        case CAMERA_MODE_OTS: return "over_shoulder";
        default: return "unknown";
        }
    }

    void tool_status(const boost::json::object*, boost::json::object& out, bool&)
    {
        out["in_world"] = in_world();
        out["startup_state"] = LLStartUp::getStartupStateString();
        out["agent_id"] = gAgent.getID().asString();
        std::string name;
        LLAgentUI::buildFullname(name);
        if (name.empty())
        {
            name = gAgentUsername;
        }
        out["agent_name"] = name;
        LLViewerRegion* region = gAgent.getRegion();
        if (region)
        {
            out["region"] = region->getName();
            out["position"] = vec3_json(gAgent.getPositionAgent());
        }
        out["look"] = vec3_json(gAgent.getAtAxis());
        out["camera_mode"] = camera_mode_name();
    }

    void tool_nearby(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "nearby");
        const F32 radius = (F32)llclamp(jreal(args, "radius", 32.0), 1.0, 256.0);
        const S32 limit = (S32)llclamp(jreal(args, "limit", 50.0), 1.0, (F64)kMaxNearby);
        const LLVector3d here = gAgent.getPositionGlobal();
        boost::json::array items;
        const S32 count = gObjectList.getNumObjects();
        for (S32 i = 0; i < count && (S32)items.size() < limit; ++i)
        {
            LLViewerObject* obj = gObjectList.getObject(i);
            if (!obj || obj->isDead())
            {
                continue;
            }
            const bool avatar = obj->isAvatar();
            if (!avatar && obj->getParent())
            {
                continue;
            }
            const F32 distance = (F32)(obj->getPositionGlobal() - here).magVec();
            if (distance > radius)
            {
                continue;
            }
            boost::json::object item;
            item["id"] = obj->getID().asString();
            item["name"] = object_label(obj);
            item["kind"] = avatar ? "avatar" : "object";
            item["distance"] = distance;
            item["position"] = vec3_json(obj->getPositionRegion());
            if (!avatar)
            {
                boost::json::array links = link_ids(obj);
                item["linkset"] = links.size() > 1;
            }
            items.push_back(std::move(item));
        }
        out["world_text_untrusted"] = true;
        out["items"] = std::move(items);
    }

    void tool_selection(const boost::json::object*, boost::json::object& out, bool&)
    {
        boost::json::array items;
        LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
        if (selection)
        {
            for (LLObjectSelection::iterator it = selection->begin(); it != selection->end(); ++it)
            {
                LLViewerObject* obj = (*it)->getObject();
                if (!obj)
                {
                    continue;
                }
                boost::json::object item;
                item["id"] = obj->getID().asString();
                item["name"] = object_label(obj);
                boost::json::array links = link_ids(obj);
                if (links.size() > 1)
                {
                    item["links"] = std::move(links);
                }
                items.push_back(std::move(item));
            }
        }
        out["items"] = std::move(items);
    }

    void tool_chat_recent(const boost::json::object* args, boost::json::object& out, bool&)
    {
        const S32 limit = (S32)llclamp(jreal(args, "limit", 40.0), 1.0, (F64)kMaxChat);
        boost::json::array lines;
        std::lock_guard<std::mutex> lock(gChatMu);
        const S32 start = (S32)gChat.size() > limit ? (S32)gChat.size() - limit : 0;
        for (S32 i = start; i < (S32)gChat.size(); ++i)
        {
            const ChatLine& line = gChat[(size_t)i];
            boost::json::object item;
            item["kind"] = line.kind;
            item["from"] = line.from;
            item["from_id"] = line.from_id.asString();
            item["text"] = line.text;
            lines.push_back(std::move(item));
        }
        out["world_text_untrusted"] = true;
        out["lines"] = std::move(lines);
    }

    void tool_camera(const boost::json::object*, boost::json::object& out, bool&)
    {
        out["mode"] = camera_mode_name();
        out["mouselook"] = gAgentCamera.cameraMouselook();
        LLViewerRegion* region = gAgent.getRegion();
        if (region)
        {
            out["position"] = vec3_json(region->getPosRegionFromGlobal(gAgentCamera.getCameraPositionGlobal()));
            out["focus"] = vec3_json(region->getPosRegionFromGlobal(gAgentCamera.getFocusGlobal()));
        }
    }

    void tool_chat(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "chat");
        const std::string text = jstr(args, "text");
        if (text.empty())
        {
            is_error = true;
            throw std::runtime_error("chat requires text");
        }
        const F64 now = LLTimer::getElapsedSeconds();
        if (now < gLastChatTime + kChatThrottleSeconds)
        {
            is_error = true;
            throw std::runtime_error("chat is throttled");
        }
        gLastChatTime = now;
        const std::string mode = jstr(args, "mode", "normal");
        EChatType type = CHAT_TYPE_NORMAL;
        if (mode == "whisper")
        {
            type = CHAT_TYPE_WHISPER;
        }
        else if (mode == "shout")
        {
            type = CHAT_TYPE_SHOUT;
        }
        const S32 channel = (S32)jreal(args, "channel", 0.0);
        send_chat_from_viewer(text, type, channel);
        out["sent"] = true;
    }

    void tool_im(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "im");
        const std::string text = jstr(args, "text");
        if (text.empty())
        {
            is_error = true;
            throw std::runtime_error("im requires text");
        }
        LLUUID id(jstr(args, "id"));
        if (id.isNull())
        {
            const std::string name = jstr(args, "name");
            const S32 count = gObjectList.getNumObjects();
            for (S32 i = 0; i < count && id.isNull(); ++i)
            {
                LLViewerObject* obj = gObjectList.getObject(i);
                if (obj && obj->isAvatar() && object_label(obj) == name)
                {
                    id = obj->getID();
                }
            }
        }
        if (id.isNull())
        {
            is_error = true;
            throw std::runtime_error("im needs an avatar id, or a name that is currently in view");
        }
        const LLUUID session = LLIMMgr::computeSessionID(IM_NOTHING_SPECIAL, id);
        LLIMModel::sendMessage(text, session, id, IM_NOTHING_SPECIAL);
        out["sent"] = true;
        out["id"] = id.asString();
    }

    void tool_teleport(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "teleport");
        const std::string region = jstr(args, "region");
        if (region.empty())
        {
            is_error = true;
            throw std::runtime_error("teleport requires a region name");
        }
        LLSD params(LLSD::emptyArray());
        params.append(region);
        params.append(jreal(args, "x", 128.0));
        params.append(jreal(args, "y", 128.0));
        params.append(jreal(args, "z", 25.0));
        const bool ok = LLCommandDispatcher::dispatch(
            "teleport", params, LLSD(), LLGridManager::getInstance()->getGrid(),
            NULL, LLCommandHandler::NAV_TYPE_CLICKED, true);
        out["sent"] = ok;
        if (!ok)
        {
            is_error = true;
            throw std::runtime_error("teleport was not accepted");
        }
    }

    void tool_move(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "move");
        if (jbool(args, "stop", false))
        {
            gAgent.stopAutoPilot(true);
            out["stopped"] = true;
            return;
        }
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            is_error = true;
            throw std::runtime_error("no current region");
        }
        const LLVector3 local((F32)jreal(args, "x", 0.0), (F32)jreal(args, "y", 0.0), (F32)jreal(args, "z", 0.0));
        gAgent.startAutoPilotGlobal(
            region->getPosGlobalFromRegion(local),
            "mcp",
            NULL,
            NULL,
            NULL,
            (F32)jreal(args, "stop_distance", 1.5),
            0.03f,
            jbool(args, "allow_flying", true));
        out["started"] = true;
    }

    void tool_sit(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "sit");
        LLViewerObject* obj = object_by_id(jstr(args, "id"), is_error);
        if (!obj->getRegion() || obj->getPCode() != LL_PCODE_VOLUME)
        {
            is_error = true;
            throw std::runtime_error("sit target is not a prim");
        }
        gMessageSystem->newMessageFast(_PREHASH_AgentRequestSit);
        gMessageSystem->nextBlockFast(_PREHASH_AgentData);
        gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        gMessageSystem->nextBlockFast(_PREHASH_TargetObject);
        gMessageSystem->addUUIDFast(_PREHASH_TargetID, obj->getID());
        gMessageSystem->addVector3Fast(_PREHASH_Offset, LLVector3::zero);
        obj->getRegion()->sendReliableMessage();
        out["sent"] = true;
    }

    void tool_stand(const boost::json::object*, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "stand");
        gAgent.setControlFlags(AGENT_CONTROL_STAND_UP);
        out["sent"] = true;
    }

    void tool_touch(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "touch");
        LLViewerObject* obj = object_by_id(jstr(args, "id"), is_error);
        LLPickInfo pick;
        pick.mPickType = LLPickInfo::PICK_OBJECT;
        pick.mObjectID = obj->getID();
        pick.mObjectFace = (S32)jreal(args, "face", 0.0);
        send_ObjectGrab_message(obj, pick, LLVector3::zero);
        send_ObjectDeGrab_message(obj, pick);
        out["sent"] = true;
    }

    void tool_look_at(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "look_at");
        LLViewerObject* obj = NULL;
        const std::string id_text = jstr(args, "id");
        if (!id_text.empty())
        {
            obj = object_by_id(id_text, is_error);
        }
        const LLVector3 pos((F32)jreal(args, "x", 0.0), (F32)jreal(args, "y", 0.0), (F32)jreal(args, "z", 0.0));
        gAgentCamera.setLookAt(LOOKAT_TARGET_SELECT, obj, pos);
        out["sent"] = true;
    }

    void tool_camera_set(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "camera_set");
        if (jbool(args, "reset", false))
        {
            gAgentCamera.changeCameraToDefault();
            out["reset"] = true;
            return;
        }
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            is_error = true;
            throw std::runtime_error("no current region");
        }
        const LLVector3d cam = region->getPosGlobalFromRegion(LLVector3(
            (F32)jreal(args, "x", 0.0), (F32)jreal(args, "y", 0.0), (F32)jreal(args, "z", 0.0)));
        const LLVector3d focus = region->getPosGlobalFromRegion(LLVector3(
            (F32)jreal(args, "focus_x", 0.0), (F32)jreal(args, "focus_y", 0.0), (F32)jreal(args, "focus_z", 0.0)));
        gAgentCamera.setCameraPosAndFocusGlobal(cam, focus, LLUUID::null);
        out["sent"] = true;
    }

    void tool_select(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "select");
        LLViewerObject* obj = object_by_id(jstr(args, "id"), is_error);
        LLSelectMgr::getInstance()->deselectAll();
        LLSelectMgr::getInstance()->selectObjectAndFamily(obj);
        out["selected"] = obj->getID().asString();
    }

    struct AsyncJob
    {
        bool done = false;
        bool ok = false;
        std::string detail;
        std::string item_id;
        std::string source;
    };

    std::mutex gAsyncMu;
    std::map<std::string, AsyncJob> gAsync;

    void finish_async(const std::string& job, bool ok, const std::string& detail, const std::string& item_id, const std::string& source = std::string())
    {
        std::lock_guard<std::mutex> lock(gAsyncMu);
        AsyncJob& slot = gAsync[job];
        slot.done = true;
        slot.ok = ok;
        slot.detail = detail;
        slot.item_id = item_id;
        slot.source = source;
        if (gAsync.size() > 32)
        {
            auto oldest = gAsync.begin();
            if (oldest->first == job)
            {
                ++oldest;
            }
            if (oldest != gAsync.end())
            {
                gAsync.erase(oldest);
            }
        }
    }

    LLViewerInventoryItem* inventory_item(const std::string& name_or_id, bool& is_error)
    {
        LLUUID id(name_or_id);
        if (id.notNull())
        {
            LLViewerInventoryItem* item = gInventory.getItem(id);
            if (!item)
            {
                is_error = true;
                throw std::runtime_error("inventory item was not found");
            }
            return item;
        }
        if (name_or_id.empty())
        {
            is_error = true;
            throw std::runtime_error("inventory item name or id is required");
        }
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH);
        LLViewerInventoryItem* found = nullptr;
        S32 matches = 0;
        for (LLViewerInventoryItem* item : items)
        {
            if (item && item->getName() == name_or_id)
            {
                found = item;
                ++matches;
            }
        }
        if (matches != 1)
        {
            is_error = true;
            throw std::runtime_error(matches == 0 ? "inventory item was not found" : "inventory name matches more than one item; pass an id");
        }
        return found;
    }

    LLPCode shape_pcode(const std::string& shape, bool& is_error)
    {
        if (shape == "box" || shape == "cube") return LL_PCODE_CUBE;
        if (shape == "sphere") return LL_PCODE_SPHERE;
        if (shape == "cylinder") return LL_PCODE_CYLINDER;
        if (shape == "prism") return LL_PCODE_PRISM;
        if (shape == "cone") return LL_PCODE_CONE;
        if (shape == "torus") return LL_PCODE_TORUS;
        if (shape == "pyramid") return LL_PCODE_PYRAMID;
        is_error = true;
        throw std::runtime_error("shape must be box, sphere, cylinder, prism, cone, torus, or pyramid");
    }

    void pack_new_prim(LLPCode pcode, LLVolumeParams& volume_params, LLQuaternion& rotation, LLPCode& volume_pcode)
    {
        rotation.loadIdentity();
        volume_pcode = LL_PCODE_VOLUME;
        switch (pcode)
        {
        case LL_PCODE_SPHERE:
            rotation.setQuat(90.f * DEG_TO_RAD, LLVector3::y_axis);
            volume_params.setType(LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE);
            volume_params.setRatio(1, 1);
            break;
        case LL_PCODE_TORUS:
            rotation.setQuat(90.f * DEG_TO_RAD, LLVector3::y_axis);
            volume_params.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_CIRCLE);
            volume_params.setRatio(1.f, 0.25f);
            break;
        case LL_PCODE_PRISM:
            volume_params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
            volume_params.setRatio(0, 1);
            volume_params.setShear(-0.5f, 0);
            break;
        case LL_PCODE_PYRAMID:
            volume_params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
            volume_params.setRatio(0, 0);
            break;
        case LL_PCODE_CYLINDER:
            volume_params.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE);
            volume_params.setRatio(1, 1);
            break;
        case LL_PCODE_CONE:
            volume_params.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE);
            volume_params.setRatio(0, 0);
            break;
        case LL_PCODE_CUBE:
        default:
            volume_params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
            volume_params.setRatio(1, 1);
            break;
        }
        volume_params.setBeginAndEndS(0.f, 1.f);
        volume_params.setBeginAndEndT(0.f, 1.f);
    }

    void tool_rez_prim(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "rez_prim");
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            is_error = true;
            throw std::runtime_error("no current region");
        }
        const LLPCode pcode = shape_pcode(jstr(args, "shape", "box"), is_error);
        const F32 size = (F32)llclamp(jreal(args, "size", 0.5), 0.01, 64.0);
        LLVector3 scale(size, size, size);
        if (jhas(args, "scale_x"))
        {
            scale.set((F32)jreal(args, "scale_x", size), (F32)jreal(args, "scale_y", size), (F32)jreal(args, "scale_z", size));
        }
        const LLVector3 pos((F32)jreal(args, "x", 128.0), (F32)jreal(args, "y", 128.0), (F32)jreal(args, "z", 25.0));
        LLVolumeParams volume_params;
        LLQuaternion rotation;
        LLPCode volume_pcode = LL_PCODE_VOLUME;
        pack_new_prim(pcode, volume_params, rotation, volume_pcode);

        U32 flags = FLAGS_CREATE_SELECTED;
        if (jbool(args, "physics", false))
        {
            flags |= FLAGS_USE_PHYSICS;
        }
        gMessageSystem->newMessageFast(_PREHASH_ObjectAdd);
        gMessageSystem->nextBlockFast(_PREHASH_AgentData);
        gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        gMessageSystem->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
        gMessageSystem->nextBlockFast(_PREHASH_ObjectData);
        gMessageSystem->addU8Fast(_PREHASH_Material, LL_MCODE_WOOD);
        gMessageSystem->addU32Fast(_PREHASH_AddFlags, flags);
        LLVolumeMessage::packVolumeParams(&volume_params, gMessageSystem);
        gMessageSystem->addU8Fast(_PREHASH_PCode, volume_pcode);
        gMessageSystem->addVector3Fast(_PREHASH_Scale, scale);
        gMessageSystem->addQuatFast(_PREHASH_Rotation, rotation);
        gMessageSystem->addVector3Fast(_PREHASH_RayStart, pos);
        gMessageSystem->addVector3Fast(_PREHASH_RayEnd, pos);
        gMessageSystem->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
        gMessageSystem->addU8Fast(_PREHASH_RayEndIsIntersection, (U8)0);
        gMessageSystem->addU8Fast(_PREHASH_State, (U8)0);
        gMessageSystem->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        gMessageSystem->sendReliable(region->getHost());
        LLSelectMgr::getInstance()->deselectAll();
        if (gViewerWindow && gViewerWindow->getWindow())
        {
            gViewerWindow->getWindow()->incBusyCount();
        }
        out["sent"] = true;
        out["note"] = "The new prim is selected when the region sends it back. Call selection to read its UUID.";
    }

    void tool_rez_item(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "rez_item");
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            is_error = true;
            throw std::runtime_error("no current region");
        }
        LLViewerInventoryItem* item = inventory_item(jstr(args, "id").empty() ? jstr(args, "name") : jstr(args, "id"), is_error);
        if (item->getType() != LLAssetType::AT_OBJECT)
        {
            is_error = true;
            throw std::runtime_error("inventory item is not an object");
        }
        const LLVector3 pos((F32)jreal(args, "x", 128.0), (F32)jreal(args, "y", 128.0), (F32)jreal(args, "z", 25.0));
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_RezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
        msg->nextBlock("RezData");
        msg->addUUIDFast(_PREHASH_FromTaskID, LLUUID::null);
        msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
        msg->addVector3Fast(_PREHASH_RayStart, pos);
        msg->addVector3Fast(_PREHASH_RayEnd, pos);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->addBOOLFast(_PREHASH_RayEndIsIntersection, false);
        msg->addBOOLFast(_PREHASH_RezSelected, true);
        msg->addBOOLFast(_PREHASH_RemoveItem, false);
        pack_permissions_slam(msg, item->getFlags(), item->getPermissions());
        msg->nextBlockFast(_PREHASH_InventoryData);
        item->packMessage(msg);
        msg->sendReliable(region->getHost());
        out["sent"] = true;
        out["item_id"] = item->getUUID().asString();
    }

    void tool_take(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "take");
        LLViewerObject* obj = object_by_id(jstr(args, "id"), is_error);
        if (!obj->permYouOwner())
        {
            is_error = true;
            throw std::runtime_error("you do not own this object");
        }
        LLSelectMgr::getInstance()->deselectAll();
        LLSelectMgr::getInstance()->selectObjectAndFamily(obj->getRootEdit() ? obj->getRootEdit() : obj);
        if (jbool(args, "copy", false))
        {
            handle_take_copy();
        }
        else
        {
            handle_take(false);
        }
        out["sent"] = true;
    }

    LLViewerJointAttachment* attachment_point(const std::string& name)
    {
        if (!isAgentAvatarValid() || name.empty())
        {
            return nullptr;
        }
        for (auto& entry : gAgentAvatarp->mAttachmentPoints)
        {
            if (entry.second && LLStringUtil::compareInsensitive(entry.second->getName(), name) == 0)
            {
                return entry.second;
            }
        }
        return nullptr;
    }

    void tool_attach_points(const boost::json::object*, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "attach_points");
        boost::json::array points;
        if (isAgentAvatarValid())
        {
            for (auto& entry : gAgentAvatarp->mAttachmentPoints)
            {
                if (!entry.second)
                {
                    continue;
                }
                boost::json::object item;
                item["name"] = entry.second->getName();
                item["attached"] = entry.second->getNumObjects();
                points.push_back(std::move(item));
            }
        }
        out["points"] = std::move(points);
    }

    void tool_attach(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "attach");
        LLViewerInventoryItem* item = inventory_item(jstr(args, "id").empty() ? jstr(args, "name") : jstr(args, "id"), is_error);
        const std::string point_name = jstr(args, "point");
        LLViewerJointAttachment* point = attachment_point(point_name);
        if (!point_name.empty() && !point)
        {
            is_error = true;
            throw std::runtime_error("unknown attachment point");
        }
        rez_attachment(item, point, false);
        out["sent"] = true;
        out["item_id"] = item->getUUID().asString();
    }

    void tool_detach(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "detach");
        LLViewerObject* obj = object_by_id(jstr(args, "id"), is_error);
        const LLUUID item_id = obj->getAttachmentItemID();
        if (item_id.isNull())
        {
            is_error = true;
            throw std::runtime_error("object is not an attachment");
        }
        LLAppearanceMgr::instance().removeItemFromAvatar(item_id);
        out["sent"] = true;
        out["item_id"] = item_id.asString();
    }

    void tool_edit(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "edit");
        LLViewerObject* obj = object_by_id(jstr(args, "id"), is_error);
        LLSelectMgr::getInstance()->deselectAll();
        LLSelectMgr::getInstance()->selectObjectOnly(obj);
        U8 updates = 0;
        if (jhas(args, "x") && jhas(args, "y") && jhas(args, "z"))
        {
            const LLVector3 pos((F32)jreal(args, "x", 0), (F32)jreal(args, "y", 0), (F32)jreal(args, "z", 0));
            LLViewerObject* root = obj->getRootEdit();
            if (root && root != obj && !root->isAvatar())
            {
                LLVector3 local = pos - root->getPositionRegion();
                local = local * ~root->getRotationRegion();
                obj->setPositionParent(local);
            }
            else
            {
                obj->setPositionEdit(pos);
            }
            updates |= UPD_POSITION;
        }
        if (jhas(args, "scale_x") || jhas(args, "size"))
        {
            const F32 size = (F32)jreal(args, "size", obj->getScale().mV[VX]);
            obj->setScale(LLVector3(
                (F32)jreal(args, "scale_x", size),
                (F32)jreal(args, "scale_y", size),
                (F32)jreal(args, "scale_z", size)), false);
            updates |= UPD_SCALE;
        }
        if (jhas(args, "rot_x") || jhas(args, "rot_y") || jhas(args, "rot_z"))
        {
            LLQuaternion rotation;
            rotation.setQuat(
                (F32)jreal(args, "rot_x", 0) * DEG_TO_RAD,
                (F32)jreal(args, "rot_y", 0) * DEG_TO_RAD,
                (F32)jreal(args, "rot_z", 0) * DEG_TO_RAD);
            obj->setRotation(rotation);
            updates |= UPD_ROTATION | UPD_POSITION;
        }
        if (updates)
        {
            LLSelectMgr::getInstance()->sendMultipleUpdate(updates);
        }
        if (jhas(args, "name"))
        {
            LLSelectMgr::getInstance()->selectionSetObjectName(jstr(args, "name"));
        }
        if (jhas(args, "description"))
        {
            LLSelectMgr::getInstance()->selectionSetObjectDescription(jstr(args, "description"));
        }
        out["sent"] = true;
        out["id"] = obj->getID().asString();
    }

    void tool_link(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "link");
        if (!args)
        {
            is_error = true;
            throw std::runtime_error("link requires ids");
        }
        auto it = args->find("ids");
        if (it == args->end() || !it->value().is_array() || it->value().as_array().size() < 2)
        {
            is_error = true;
            throw std::runtime_error("link requires at least two ids; the last id becomes the root");
        }
        LLSelectMgr::getInstance()->deselectAll();
        for (const boost::json::value& value : it->value().as_array())
        {
            if (!value.is_string())
            {
                continue;
            }
            LLViewerObject* obj = object_by_id(std::string(value.as_string()), is_error);
            LLSelectMgr::getInstance()->selectObjectAndFamily(obj, true);
        }
        const bool ok = LLSelectMgr::getInstance()->linkObjects();
        out["sent"] = ok;
    }

    void tool_unlink(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "unlink");
        LLViewerObject* obj = object_by_id(jstr(args, "id"), is_error);
        LLSelectMgr::getInstance()->deselectAll();
        LLSelectMgr::getInstance()->selectObjectAndFamily(obj->getRootEdit() ? obj->getRootEdit() : obj);
        const bool ok = LLSelectMgr::getInstance()->unlinkObjects();
        out["sent"] = ok;
    }

    void tool_contents(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "contents");
        LLViewerObject* obj = object_by_id(jstr(args, "id"), is_error);
        LLInventoryObject::object_list_t objects;
        obj->getInventoryContents(objects);
        if (objects.empty())
        {
            if (!obj->isInventoryPending())
            {
                obj->requestInventory();
            }
            out["pending"] = true;
            out["items"] = boost::json::array();
            return;
        }
        boost::json::array items;
        for (const LLPointer<LLInventoryObject>& entry : objects)
        {
            LLInventoryItem* item = dynamic_cast<LLInventoryItem*>(entry.get());
            if (!item)
            {
                continue;
            }
            boost::json::object row;
            row["id"] = item->getUUID().asString();
            row["name"] = item->getName();
            row["type"] = std::string(LLAssetType::lookup(item->getType()));
            items.push_back(std::move(row));
        }
        out["pending"] = false;
        out["items"] = std::move(items);
    }

    void tool_script_create(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "script_create");
        LLViewerObject* obj = object_by_id(jstr(args, "id"), is_error);
        const std::string name = jstr(args, "name", "New Script");
        const bool lua = jstr(args, "language", "lsl") == "lua";
        LLPermissions perm;
        perm.init(gAgent.getID(), gAgent.getID(), LLUUID::null, LLUUID::null);
        perm.initMasks(PERM_ALL, PERM_ALL, PERM_COPY, PERM_NONE, PERM_COPY | PERM_MODIFY | PERM_TRANSFER);
        LLSD params;
        params["enabled"] = true;
        params["vm"] = lua ? "luau" : "mono";
        const std::string job = LLUUID::generateNewID().asString();
        {
            std::lock_guard<std::mutex> lock(gAsyncMu);
            gAsync[job] = AsyncJob();
        }
        obj->createInventoryItem(
            LLAssetType::AT_LSL_TEXT,
            LLInventoryType::IT_LSL,
            lua ? (U8)SST_LUA : (U8)SST_LSL,
            name,
            "script",
            perm,
            params,
            [job](bool success, const LLSD& response)
            {
                std::string item_id;
                if (response.has("item_id"))
                {
                    item_id = response["item_id"].asString();
                }
                finish_async(job, success, response["message"].asString(), item_id);
            });
        out["job"] = job;
        out["pending"] = true;
    }

    void tool_script_save(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "script_save");
        LLViewerObject* obj = object_by_id(jstr(args, "id"), is_error);
        const LLUUID item_id(jstr(args, "item_id"));
        if (item_id.isNull())
        {
            is_error = true;
            throw std::runtime_error("script_save requires item_id");
        }
        const std::string source = jstr(args, "source");
        if (source.empty())
        {
            is_error = true;
            throw std::runtime_error("script_save requires source");
        }
        if (!obj->getRegion())
        {
            is_error = true;
            throw std::runtime_error("object has no region");
        }
        const std::string url = obj->getRegion()->getCapability("UpdateScriptTask");
        if (url.empty())
        {
            is_error = true;
            throw std::runtime_error("UpdateScriptTask is not available");
        }
        const std::string vm = jstr(args, "vm", "mono");
        const bool running = jbool(args, "running", true);
        const std::string job = LLUUID::generateNewID().asString();
        {
            std::lock_guard<std::mutex> lock(gAsyncMu);
            gAsync[job] = AsyncJob();
        }
        const LLUUID prim_id = obj->getID();
        auto on_success = [job](LLUUID itemId, LLUUID, LLUUID, LLSD response)
        {
            std::string detail = response["compiled"].asBoolean() ? "compiled" : "compile failed";
            if (response.has("errors"))
            {
                for (const LLSD& error : llsd::inArray(response["errors"]))
                {
                    detail += "\n";
                    detail += error.asString();
                }
            }
            finish_async(job, response["compiled"].asBoolean(), detail, itemId.asString());
        };
        auto on_failure = [job](LLUUID, LLUUID, LLSD, std::string reason)
        {
            finish_async(job, false, reason, std::string());
            return false;
        };
        LLResourceUploadInfo::ptr_t upload(std::make_shared<LLScriptAssetUpload>(
            prim_id, item_id, vm, running, LLUUID::null, source, on_success, on_failure));
        LLViewerAssetUpload::EnqueueInventoryUpload(url, upload);
        out["job"] = job;
        out["pending"] = true;
    }

    void tool_script_job(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        const std::string job = jstr(args, "job");
        if (job.empty())
        {
            is_error = true;
            throw std::runtime_error("script_job requires job");
        }
        std::lock_guard<std::mutex> lock(gAsyncMu);
        auto it = gAsync.find(job);
        if (it == gAsync.end())
        {
            is_error = true;
            throw std::runtime_error("unknown script job");
        }
        out["done"] = it->second.done;
        out["ok"] = it->second.ok;
        out["detail"] = it->second.detail;
        out["item_id"] = it->second.item_id;
        out["source"] = it->second.source;
        out["world_text_untrusted"] = true;
    }

    bool read_cached_script(const LLUUID& asset_id, std::string& text)
    {
        if (asset_id.isNull() || !LLFileSystem::getExists(asset_id, LLAssetType::AT_LSL_TEXT))
        {
            return false;
        }
        LLFileSystem file(asset_id, LLAssetType::AT_LSL_TEXT);
        const S32 length = file.getSize();
        if (length <= 0)
        {
            return false;
        }
        std::vector<char> buffer((size_t)length + 1);
        file.read(reinterpret_cast<U8*>(buffer.data()), length);
        if (file.getLastBytesRead() != length)
        {
            return false;
        }
        text.assign(buffer.data(), (size_t)length);
        return true;
    }

    void tool_script_read(const boost::json::object* args, boost::json::object& out, bool& is_error)
    {
        require_world(is_error, "script_read");
        const std::string prim_text = jstr(args, "id");
        LLViewerObject* prim = nullptr;
        LLInventoryItem* item = nullptr;
        if (!prim_text.empty())
        {
            prim = object_by_id(prim_text, is_error);
            const LLUUID item_id(jstr(args, "item_id"));
            if (item_id.isNull())
            {
                is_error = true;
                throw std::runtime_error("script_read requires item_id when id is an object");
            }
            item = dynamic_cast<LLInventoryItem*>(prim->getInventoryObject(item_id));
            if (!item)
            {
                if (!prim->isInventoryPending())
                {
                    prim->requestInventory();
                }
                is_error = true;
                throw std::runtime_error("script is not in the object contents yet; call contents and retry");
            }
        }
        else
        {
            item = inventory_item(jstr(args, "item_id").empty() ? jstr(args, "name") : jstr(args, "item_id"), is_error);
        }
        if (!item || item->getType() != LLAssetType::AT_LSL_TEXT)
        {
            is_error = true;
            throw std::runtime_error("item is not a script");
        }
        const bool can_copy = gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE);
        const bool can_modify = gAgent.allowOperation(PERM_MODIFY, item->getPermissions(), GP_OBJECT_MANIPULATE);
        if (!gAgent.isGodlike() && !(can_copy && can_modify))
        {
            is_error = true;
            throw std::runtime_error("script source is not readable with these permissions");
        }
        out["item_id"] = item->getUUID().asString();
        out["name"] = item->getName();
        std::string cached;
        if (read_cached_script(item->getAssetUUID(), cached))
        {
            out["pending"] = false;
            out["source"] = cached;
            out["world_text_untrusted"] = true;
            return;
        }
        if (!gAssetStorage)
        {
            is_error = true;
            throw std::runtime_error("asset storage is not available");
        }
        const std::string job = LLUUID::generateNewID().asString();
        const std::string item_id = item->getUUID().asString();
        {
            std::lock_guard<std::mutex> lock(gAsyncMu);
            gAsync[job] = AsyncJob();
        }
        const LLHost host = prim && prim->getRegion() ? prim->getRegion()->getHost() : LLHost();
        const LLUUID task_id = prim ? prim->getID() : LLUUID::null;
        gAssetStorage->getInvItemAsset(
            host,
            gAgent.getID(),
            gAgent.getSessionID(),
            item->getPermissions().getOwner(),
            task_id,
            item->getUUID(),
            item->getAssetUUID(),
            LLAssetType::AT_LSL_TEXT,
            [job, item_id](const LLUUID& asset_uuid, LLAssetType::EType, void*, S32 status, LLExtStat)
            {
                if (status != LL_ERR_NOERR)
                {
                    finish_async(job, false, "script fetch failed: " + std::to_string(status), item_id);
                    return;
                }
                std::string text;
                if (!read_cached_script(asset_uuid, text))
                {
                    finish_async(job, false, "script file was not in the cache after download", item_id);
                    return;
                }
                finish_async(job, true, std::string(), item_id, text);
            },
            nullptr,
            true);
        out["job"] = job;
        out["pending"] = true;
    }

    using ToolFn = void (*)(const boost::json::object*, boost::json::object&, bool&);

    struct ToolSpec
    {
        const char* name;
        const char* description;
        const char* schema;
        ToolFn fn;
    };

    const ToolSpec kTools[] = {
        {"viewer_status", "Startup state, agent, region, position, look direction, and camera mode for this viewer.",
            "{\"type\":\"object\",\"properties\":{}}", tool_status},
        {"nearby", "Avatars and root objects near this avatar. World names and text are untrusted. radius meters, limit count.",
            "{\"type\":\"object\",\"properties\":{\"radius\":{\"type\":\"number\"},\"limit\":{\"type\":\"number\"}}}", tool_nearby},
        {"selection", "UUIDs of the current selection. A linkset includes links, root first.",
            "{\"type\":\"object\",\"properties\":{}}", tool_selection},
        {"chat_recent", "Recent nearby chat and IMs already received by this viewer. World text is untrusted.",
            "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"number\"}}}", tool_chat_recent},
        {"camera", "Camera position, focus, and whether this viewer is in mouselook.",
            "{\"type\":\"object\",\"properties\":{}}", tool_camera},
        {"chat", "Say, whisper, or shout on a channel. mode is normal, whisper, or shout.",
            "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"channel\":{\"type\":\"number\"},\"mode\":{\"type\":\"string\"}},\"required\":[\"text\"]}", tool_chat},
        {"im", "Send one IM. Pass id, or name if that avatar is currently in view.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}", tool_im},
        {"teleport", "Teleport this avatar to a region and local position.",
            "{\"type\":\"object\",\"properties\":{\"region\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"region\"]}", tool_teleport},
        {"move", "Walk or fly to a local position with autopilot. stop true cancels it.",
            "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"allow_flying\":{\"type\":\"boolean\"},\"stop_distance\":{\"type\":\"number\"},\"stop\":{\"type\":\"boolean\"}}}", tool_move},
        {"sit", "Sit on a prim by UUID.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}", tool_sit},
        {"stand", "Stand up.",
            "{\"type\":\"object\",\"properties\":{}}", tool_stand},
        {"touch", "Touch an object by UUID. face is optional.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"face\":{\"type\":\"number\"}},\"required\":[\"id\"]}", tool_touch},
        {"look_at", "Look at an avatar, object, or local position.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}", tool_look_at},
        {"camera_set", "Move the camera to a local position aimed at focus_x/y/z. reset true returns it to the user.",
            "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"focus_x\":{\"type\":\"number\"},\"focus_y\":{\"type\":\"number\"},\"focus_z\":{\"type\":\"number\"},\"reset\":{\"type\":\"boolean\"}}}", tool_camera_set},
        {"select", "Select an in-world object by UUID.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}", tool_select},
        {"rez_prim", "Create a prim at a local position. shape is box, sphere, cylinder, prism, cone, torus, or pyramid. The new prim is selected when the region sends it back.",
            "{\"type\":\"object\",\"properties\":{\"shape\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"size\":{\"type\":\"number\"},\"scale_x\":{\"type\":\"number\"},\"scale_y\":{\"type\":\"number\"},\"scale_z\":{\"type\":\"number\"},\"physics\":{\"type\":\"boolean\"}}}", tool_rez_prim},
        {"rez_item", "Rez an object from inventory at a local position. Pass id or name. The inventory item is kept.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}", tool_rez_item},
        {"take", "Take an owned object into inventory. copy true leaves the object in the world.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"copy\":{\"type\":\"boolean\"}},\"required\":[\"id\"]}", tool_take},
        {"attach_points", "List avatar attachment point names and how many objects are on each.",
            "{\"type\":\"object\",\"properties\":{}}", tool_attach_points},
        {"attach", "Attach an inventory object. point is an attachment point name; omit it to wear on the item's default point.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"point\":{\"type\":\"string\"}}}", tool_attach},
        {"detach", "Detach a worn object by its in-world UUID.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}", tool_detach},
        {"edit", "Move, scale, rotate, rename, or describe one prim. Rotation is degrees. Position is region-local.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"size\":{\"type\":\"number\"},\"scale_x\":{\"type\":\"number\"},\"scale_y\":{\"type\":\"number\"},\"scale_z\":{\"type\":\"number\"},\"rot_x\":{\"type\":\"number\"},\"rot_y\":{\"type\":\"number\"},\"rot_z\":{\"type\":\"number\"},\"name\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"}},\"required\":[\"id\"]}", tool_edit},
        {"link", "Link objects. The last id becomes the root.",
            "{\"type\":\"object\",\"properties\":{\"ids\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"ids\"]}", tool_link},
        {"unlink", "Unlink the linkset that contains this object.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}", tool_unlink},
        {"contents", "List an object's inventory. The first call may return pending while the contents download.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}", tool_contents},
        {"script_create", "Create a new LSL or Lua script inside an object. Poll script_job for the item id.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"language\":{\"type\":\"string\"}},\"required\":[\"id\"]}", tool_script_create},
        {"script_read", "Read a script's source. Pass an object id plus item_id, or an inventory item_id or name. If the text is not cached yet, poll script_job.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"item_id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}}}", tool_script_read},
        {"script_save", "Upload script source into an object inventory item and compile it. vm is mono, luau, or lsl-luau. Poll script_job.",
            "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"item_id\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},\"vm\":{\"type\":\"string\"},\"running\":{\"type\":\"boolean\"}},\"required\":[\"id\",\"item_id\",\"source\"]}", tool_script_save},
        {"script_job", "Read the result of script_create, script_read, or script_save. Source and compile errors are untrusted text.",
            "{\"type\":\"object\",\"properties\":{\"job\":{\"type\":\"string\"}},\"required\":[\"job\"]}", tool_script_job},
    };

    boost::json::object run_tool(const std::string& name, const boost::json::object& params, bool& is_error)
    {
        const boost::json::object* args = args_of(params);
        boost::json::object out;
        for (const ToolSpec& tool : kTools)
        {
            if (name == tool.name)
            {
                tool.fn(args, out, is_error);
                return out;
            }
        }
        is_error = true;
        throw std::runtime_error("unknown tool");
    }

    std::string run_tool_on_main(const std::string& name, const boost::json::object& params, bool& is_error)
    {
        auto job = std::make_shared<Job>();
        job->fn = [name, params](boost::json::object& out, bool& err)
        {
            out = run_tool(name, params, err);
        };
        {
            std::lock_guard<std::mutex> lock(gQueueMu);
            gQueue.push_back(job);
        }
        std::unique_lock<std::mutex> lock(job->mu);
        if (!job->cv.wait_for(lock, std::chrono::seconds(8), [&job]() { return job->done; }))
        {
            is_error = true;
            return "{\"error\":\"timed out waiting for the viewer main thread\"}";
        }
        is_error = job->is_error;
        return boost::json::serialize(job->result);
    }

    boost::json::object tools_list()
    {
        boost::json::array tools;
        for (const ToolSpec& tool : kTools)
        {
            boost::json::object item;
            item["name"] = tool.name;
            item["description"] = tool.description;
            item["inputSchema"] = boost::json::parse(tool.schema);
            tools.push_back(std::move(item));
        }
        boost::json::object result;
        result["tools"] = std::move(tools);
        return result;
    }

    boost::json::object dispatch(const boost::json::object& msg, bool& notification)
    {
        const std::string method = msg.if_contains("method") && msg.at("method").is_string()
            ? std::string(msg.at("method").as_string()) : std::string();
        const bool has_id = msg.if_contains("id") && !msg.at("id").is_null();
        notification = !has_id;
        boost::json::object result;
        if (method == "initialize")
        {
            result["protocolVersion"] = "2025-03-26";
            result["capabilities"] = boost::json::object{{"tools", boost::json::object{}}};
            result["serverInfo"] = boost::json::object{{"name", "megapahit"}, {"version", "1"}};
        }
        else if (method == "tools/list")
        {
            result = tools_list();
        }
        else if (method == "tools/call")
        {
            const boost::json::object empty;
            const boost::json::object& params = msg.if_contains("params") && msg.at("params").is_object()
                ? msg.at("params").as_object() : empty;
            const std::string name = params.if_contains("name") && params.at("name").is_string()
                ? std::string(params.at("name").as_string()) : std::string();
            bool is_error = false;
            std::string text;
            try
            {
                text = run_tool_on_main(name, params, is_error);
            }
            catch (const std::exception& e)
            {
                is_error = true;
                text = e.what();
            }
            boost::json::array content;
            content.push_back(boost::json::object{{"type", "text"}, {"text", text}});
            result["content"] = std::move(content);
            result["isError"] = is_error;
        }
        else if (method == "ping" || method == "notifications/initialized" || method == "notifications/cancelled")
        {
            notification = method != "ping" && !has_id;
        }
        else if (!notification)
        {
            throw std::runtime_error("method not found");
        }
        return result;
    }

    bool read_some(mcp_socket_t fd, std::string& buf)
    {
        char tmp[4096];
#if LL_WINDOWS
        const int n = recv(fd, tmp, sizeof(tmp), 0);
#else
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
#endif
        if (n <= 0)
        {
            return false;
        }
        buf.append(tmp, tmp + n);
        return true;
    }

    void send_all(mcp_socket_t fd, const std::string& data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
#if LL_WINDOWS
            const int n = ::send(fd, data.data() + sent, (int)(data.size() - sent), 0);
#else
            const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
            if (n <= 0)
            {
                return;
            }
            sent += (size_t)n;
        }
    }

    void reply_http(mcp_socket_t fd, int code, const std::string& reason, const std::string& body, const char* content_type)
    {
        std::ostringstream out;
        out << "HTTP/1.1 " << code << " " << reason << "\r\n"
            << "Content-Type: " << content_type << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "\r\n"
            << body;
        send_all(fd, out.str());
    }

    void serve_client(mcp_socket_t fd)
    {
        std::string buf;
        while (buf.find("\r\n\r\n") == std::string::npos)
        {
            if (!read_some(fd, buf) || buf.size() > 1024 * 1024)
            {
                return;
            }
        }
        const size_t header_end = buf.find("\r\n\r\n");
        const std::string headers = buf.substr(0, header_end);
        const size_t line_end = headers.find("\r\n");
        const std::string request = headers.substr(0, line_end);
        if (request.find("POST /mcp") == std::string::npos)
        {
            reply_http(fd, 404, "Not Found", "", "text/plain");
            return;
        }
        std::string lower = headers;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        const size_t len_at = lower.find("content-length:");
        size_t length = 0;
        if (len_at != std::string::npos)
        {
            length = (size_t)std::strtoul(lower.c_str() + len_at + 15, nullptr, 10);
        }
        if (length > 1024 * 1024)
        {
            reply_http(fd, 413, "Payload Too Large", "", "text/plain");
            return;
        }
        std::string body = buf.substr(header_end + 4);
        while (body.size() < length)
        {
            if (!read_some(fd, body))
            {
                return;
            }
        }
        body.resize(length);
        try
        {
            boost::json::value parsed = boost::json::parse(body);
            if (!parsed.is_object())
            {
                throw std::runtime_error("request is not an object");
            }
            const boost::json::object& msg = parsed.as_object();
            bool notification = false;
            boost::json::object result = dispatch(msg, notification);
            if (notification)
            {
                reply_http(fd, 202, "Accepted", "", "text/plain");
                return;
            }
            boost::json::object response;
            response["jsonrpc"] = "2.0";
            response["id"] = msg.if_contains("id") ? msg.at("id") : boost::json::value(nullptr);
            response["result"] = std::move(result);
            reply_http(fd, 200, "OK", boost::json::serialize(response), "application/json");
        }
        catch (const std::exception& e)
        {
            boost::json::object response;
            response["jsonrpc"] = "2.0";
            response["id"] = nullptr;
            response["error"] = boost::json::object{{"code", -32603}, {"message", e.what()}};
            reply_http(fd, 200, "OK", boost::json::serialize(response), "application/json");
        }
    }

    void write_endpoint_file()
    {
        if (!gDirUtilp)
        {
            return;
        }
        const std::string path = gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + "mcp.json";
        boost::json::object doc;
        doc["url"] = std::string("http://127.0.0.1:") + llformat("%d", gPort) + "/mcp";
        const std::string body = boost::json::serialize(doc);
#if LL_WINDOWS
        FILE* fp = fopen(path.c_str(), "wb");
#else
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        FILE* fp = fd >= 0 ? fdopen(fd, "w") : nullptr;
#endif
        if (!fp)
        {
            LL_WARNS("MCP") << "Could not write " << path << LL_ENDL;
            return;
        }
        fwrite(body.data(), 1, body.size(), fp);
        fclose(fp);
#if !LL_WINDOWS
        ::chmod(path.c_str(), 0600);
#endif
        LL_INFOS("MCP") << "Listening on 127.0.0.1:" << gPort << LL_ENDL;
    }

    void remove_endpoint_file()
    {
        if (!gDirUtilp)
        {
            return;
        }
        const std::string path = gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + "mcp.json";
        LLFile::remove(path);
    }

    void listen_loop()
    {
#if LL_WINDOWS
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        gListen = ::socket(AF_INET, SOCK_STREAM, 0);
        if (gListen == MCP_INVALID_SOCKET)
        {
            gRun = false;
            return;
        }
        int yes = 1;
        ::setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)gPort);
        if (::bind(gListen, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(gListen, 4) != 0)
        {
            LL_WARNS("MCP") << "Could not listen on 127.0.0.1:" << gPort << LL_ENDL;
            mcp_close_socket(gListen);
            gListen = MCP_INVALID_SOCKET;
            gRun = false;
            return;
        }
        sockaddr_in bound;
        socklen_t bound_len = sizeof(bound);
        ::getsockname(gListen, (sockaddr*)&bound, &bound_len);
        gPort = ntohs(bound.sin_port);
        write_endpoint_file();

        while (gRun)
        {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(gListen, &fds);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000;
#if LL_WINDOWS
            const int ready = ::select(0, &fds, nullptr, nullptr, &tv);
#else
            const int ready = ::select(gListen + 1, &fds, nullptr, nullptr, &tv);
#endif
            if (!gRun || ready <= 0)
            {
                continue;
            }
            sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            mcp_socket_t client = ::accept(gListen, (sockaddr*)&client_addr, &client_len);
            if (client == MCP_INVALID_SOCKET)
            {
                continue;
            }
            if (client_addr.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
            {
                mcp_close_socket(client);
                continue;
            }
            serve_client(client);
            mcp_close_socket(client);
        }
        mcp_close_socket(gListen);
        gListen = MCP_INVALID_SOCKET;
        remove_endpoint_file();
    }

    void start_server()
    {
        if (gStarted)
        {
            return;
        }
        S32 port = 9411;
        if (gSavedSettings.controlExists("ViewerMCPPort"))
        {
            port = gSavedSettings.getS32("ViewerMCPPort");
        }
        if (port < 1 || port > 65535)
        {
            port = 9411;
        }
        gPort = (int)port;
        gRun = true;
        gStarted = true;
        gThread = std::thread(listen_loop);
    }

    void request_stop()
    {
        gRun = false;
        mcp_close_socket(gListen);
        gListen = MCP_INVALID_SOCKET;
    }

    void finish_stop()
    {
        if (!gStarted)
        {
            return;
        }
        if (gThread.joinable())
        {
            gThread.join();
        }
        gStarted = false;
        remove_endpoint_file();
    }
}

void LLViewerMCP::pump()
{
    const bool enabled = !gShutdown && gSavedSettings.getBOOL("EnableViewerMCP");
    if (gShutdown || !enabled)
    {
        if (gStarted)
        {
            request_stop();
        }
    }
    else
    {
        start_server();
    }

    std::deque<std::shared_ptr<Job>> jobs;
    {
        std::lock_guard<std::mutex> lock(gQueueMu);
        jobs.swap(gQueue);
    }
    for (const std::shared_ptr<Job>& job : jobs)
    {
        bool is_error = false;
        boost::json::object result;
        try
        {
            job->fn(result, is_error);
        }
        catch (const std::exception& e)
        {
            is_error = true;
            result["error"] = e.what();
        }
        {
            std::lock_guard<std::mutex> lock(job->mu);
            job->result = std::move(result);
            job->is_error = is_error;
            job->done = true;
        }
        job->cv.notify_one();
    }
    if ((gShutdown || !enabled) && gStarted)
    {
        finish_stop();
    }
}

void LLViewerMCP::shutdown()
{
    gShutdown = true;
    pump();
}

void LLViewerMCP::noteChat(const std::string& kind, const std::string& from, const LLUUID& from_id, const std::string& text)
{
    if (text.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(gChatMu);
    gChat.push_back(ChatLine{kind, from, from_id, text});
    while ((S32)gChat.size() > kMaxChat)
    {
        gChat.pop_front();
    }
}
