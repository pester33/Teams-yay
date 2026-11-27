#include <Geode/Geode.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/MDPopup.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/random.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;

// Geode 6 (GD 2.2091) and Geode 5 (GD 2.2081) take different time types for timeouts
static void setTimeout(web::WebRequest& req, int secs) {
#if GEODE_COMP_GD_VERSION >= 22091
    req.timeout(asp::Duration::fromSecs(secs));
#else
    req.timeout(std::chrono::seconds(secs));
#endif
}

// Supabase project for collab rooms (publishable key: safe to ship, access is limited by RLS)
static constexpr auto SUPA_URL = "https://daofdapshrvvqwdlkqae.supabase.co";
static constexpr auto SUPA_KEY = "sb_publishable_q2xRu0jn0aG8VIKWCcvc8w_QpjBM7VT";

static web::WebRequest supa() {
    web::WebRequest req;
    req.header("apikey", SUPA_KEY);
    req.header("Content-Type", "application/json");
    setTimeout(req, 20);
    return req;
}

static std::string randomString(size_t len, std::string_view alphabet) {
    std::string out;
    for (size_t i = 0; i < len; i++) out += alphabet[random::generate<uint64_t>() % alphabet.size()];
    return out;
}

static std::string playerName() {
    std::string name = GameManager::get()->m_playerName;
    return name.empty() ? "Player" : name;
}

// days since 1970-01-01 -> civil date (Howard Hinnant)
static void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2));
}

// UTC time as "2026-11-26T13:41:36Z", optionally shifted
static std::string isoNow(int64_t offsetSecs = 0) {
    int64_t t = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + offsetSecs;
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    int y; unsigned m, d;
    civilFromDays(days, y, m, d);
    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", y, m, d, rem / 3600, (rem / 60) % 60, rem % 60);
}

// Every player gets their own color, picked from their client id
static ccColor3B colorForClient(std::string const& client) {
    static constexpr ccColor3B palette[] = {
        { 255, 95, 95 }, { 90, 200, 255 }, { 120, 255, 120 }, { 255, 205, 60 },
        { 215, 125, 255 }, { 255, 145, 205 }, { 80, 255, 220 }, { 255, 160, 80 },
    };
    uint32_t h = 2166136261u;
    for (unsigned char c : client) { h ^= c; h *= 16777619u; }
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

// ----- Color channels (BG, Ground, 1-999...) live in the level settings string under kS38
// kS38 looks like "1_255_2_0_3_0_6_1000_7_1|1_0_2_102_3_255_6_1001_7_1|..."
static std::string levelColorString(LevelEditorLayer* editor) {
    if (!editor->m_levelSettings) return "";
    std::string header = editor->m_levelSettings->getSaveString();
    auto start = header.find("kS38,");
    if (start == std::string::npos) return "";
    start += 5;
    auto end = header.find(',', start);
    return header.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static int colorEntryChannel(std::string const& entry) {
    // pairs of key_value: channel id is key 6
    auto parts = utils::string::split(entry, "_");
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        if (parts[i] == "6") return utils::numFromString<int>(parts[i + 1]).unwrapOr(0);
    }
    return 0;
}

static std::map<int, std::string> splitColors(std::string const& str) {
    std::map<int, std::string> out;
    for (auto& entry : utils::string::split(str, "|")) {
        if (entry.empty()) continue;
        int id = colorEntryChannel(entry);
        if (id > 0) out[id] = entry;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The collab session: tracks every object with a shared id and syncs changes
// ---------------------------------------------------------------------------

struct Tracked {
    std::string uuid;
    std::string str; // object's save string the last time we looked
    Ref<GameObject> keep; // keeps the object alive until we notice it's gone
};

class CollabSession {
public:
    static CollabSession& get() {
        static CollabSession instance;
        return instance;
    }

    bool active = false;
    bool isHost = false;
    std::string room;
    std::string status = "Not in a room";
    std::string projectName;
    std::string lastSave = "Not saved yet";
    std::vector<std::string> members;

    // Other players in the room, with their cursor (in level coordinates)
    struct Member {
        std::string client;
        std::string name;
        float x = 0.f, y = 0.f;
        bool hasCursor = false;
        ccColor3B color;
    };
    std::vector<Member> others;

    // Objects someone else just changed, waiting to be flashed in the editor
    struct Highlight {
        std::vector<CCRect> rects;
        std::string name;
        ccColor3B color;
    };
    std::vector<Highlight> highlights;

    // "Pester placed 3, deleted 1"
    struct FeedLine {
        std::string text;
        ccColor3B color;
        float age = 0.f;
    };
    std::deque<FeedLine> feed;

    // Our own cursor, sent with every ping
    CCPoint cursor = { 0.f, 0.f };
    bool hasCursor = false;
    // where we last placed / moved an object (PC shows this as our cursor)
    CCPoint lastEditPos = { 0.f, 0.f };
    bool hasEditPos = false;

    bool isActiveFor(LevelEditorLayer* editor) const {
        return active && m_editor.lock().data() == editor;
    }

    // ----- Starting / stopping

    void host(LevelEditorLayer* editor, std::string projectName, std::function<void(bool, std::string)> done, int attempt = 0) {
        this->reset();
        m_editor = editor;
        this->scanLocal(editor, true); // give every existing object an id (nothing is sent yet)

        auto code = randomString(5, "ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
        auto ownerKey = randomString(24, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
        std::string levelName = projectName.substr(0, 40);
        auto body = matjson::makeObject({
            { "code", code },
            { "host", playerName() },
            { "level_name", levelName },
            { "snapshot", this->snapshotJson() },
            { "snapshot_event", 0 },
            { "saved_by", playerName() },
            { "objects", static_cast<int>(m_tracked.size()) },
            { "owner_key", ownerKey },
        });
        auto req = supa();
        req.header("Prefer", "return=minimal");
        req.bodyJSON(body);
        m_tasks.spawn(req.post(fmt::format("{}/rest/v1/team_projects", SUPA_URL)), [this, done, attempt, code, projectName, ownerKey](web::WebResponse res) {
            if (res.code() == 409 && attempt < 5) {
                // code already taken, try another
                if (auto ed = m_editor.lock()) return this->host(ed, projectName, done, attempt + 1);
            }
            if (!res.ok()) {
                log::warn("[teams] Couldn't create room: HTTP {} {}", res.code(), res.string().unwrapOr(""));
                this->reset();
                return done(false, fmt::format("Couldn't create the project (HTTP {})", res.code()));
            }
            room = code;
            isHost = true;
            Mod::get()->setSavedValue<std::string>("owner-" + code, ownerKey);
            active = true;
            status = "Project created";
            this->projectName = projectName;
            log::info("[teams] Hosting room {}", code);
            this->ping();
            done(true, "");
        });
    }

    void join(LevelEditorLayer* editor, std::string code, std::function<void(bool, std::string)> done) {
        this->reset();
        m_editor = editor;
        auto url = fmt::format("{}/rest/v1/team_projects?code=eq.{}&select=snapshot,snapshot_event,host,level_name", SUPA_URL, code);
        m_tasks.spawn(supa().get(url), [this, done, code](web::WebResponse res) {
            auto json = res.json().unwrapOr(matjson::Value());
            if (!res.ok() || !json.isArray() || json.size() == 0) {
                this->reset();
                return done(false, res.ok() ? "No room with that code" : fmt::format("Couldn't reach the server (HTTP {})", res.code()));
            }
            auto editor = m_editor.lock();
            if (!editor) return this->reset();

            auto snapshot = matjson::parse(json[0]["snapshot"].asString().unwrapOr("[]")).unwrapOr(matjson::Value());
            m_lastEvent = json[0]["snapshot_event"].asInt().unwrapOr(0);
            m_safeEvent = m_lastEvent;
            this->clearLevel(editor);
            std::vector<std::pair<std::string, std::string>> objects;
            std::vector<std::string> colorEntries;
            if (snapshot.isArray()) {
                for (auto& pair : snapshot) {
                    if (pair.isArray() && pair.size() == 2) {
                        auto uuid = pair[0].asString().unwrapOr("");
                        auto str = pair[1].asString().unwrapOr("");
                        if (uuid == "#colors") {
                            for (auto& [id, entry] : splitColors(str)) colorEntries.push_back(entry);
                        } else {
                            objects.push_back({ uuid, str });
                        }
                    }
                }
            }
            this->createObjects(editor, objects);
            this->applyColors(editor, colorEntries);
            m_colors = splitColors(levelColorString(editor)); // starting point: nothing to send yet
            room = code;
            isHost = false;
            active = true;
            projectName = json[0]["level_name"].asString().unwrapOr("Team level");
            status = "Loaded the saved level";
            log::info("[teams] Joined room {} ({} objects, from event {})", code, objects.size(), m_lastEvent);
            this->ping();
            done(true, "");
        });
    }

    void leave() {
        if (active) log::info("[teams] Left project {}", room);
        this->reset();
    }

    // Save right now (the Save button, and when you leave the editor).
    // Uses its own task holders so it still finishes after we leave.
    void saveNow(LevelEditorLayer* editor, bool leaving) {
        if (!this->isActiveFor(editor)) return;
        auto ops = this->scanLocal(editor, false);
        if (m_retry.size() > 0) {
            for (auto& op : ops) m_retry.push(op);
            ops = std::move(m_retry);
            m_retry = matjson::Value(std::vector<matjson::Value>{});
        }
        if (ops.size() > 0) {
            // send the last edits too, so people working right now get them
            auto body = matjson::makeObject({
                { "room", room }, { "client", m_client }, { "author", playerName() }, { "data", ops.dump(matjson::NO_INDENTATION) },
            });
            auto req = supa();
            req.header("Prefer", "return=minimal");
            req.bodyJSON(body);
            finalPushTask().spawn(req.post(fmt::format("{}/rest/v1/team_events", SUPA_URL)), [](web::WebResponse res) {
                if (!res.ok()) log::warn("[teams] Last edits failed to send: HTTP {}", res.code());
            });
            m_dirty = true;
        }
        if (!m_dirty && !leaving) {
            lastSave = "Already saved";
            return;
        }
        if (!m_dirty) return;
        auto [req, url] = this->snapshotRequest();
        bool* stillHere = leaving ? nullptr : &m_saving;
        if (stillHere) *stillHere = true;
        auto roomCode = room;
        finalSaveTask().spawn(req.patch(url), [this, leaving, roomCode](web::WebResponse res) {
            if (leaving) {
                if (res.ok()) Notification::create("Team level saved", NotificationIcon::Success)->show();
                else Notification::create(fmt::format("Couldn't save the team level (HTTP {})", res.code()), NotificationIcon::Error, 3.f)->show();
                return;
            }
            if (room != roomCode) return;
            m_saving = false;
            this->onSaved(res);
        });
        m_lastSaveTick = m_ticks;
    }

    // ----- Called about once a second by the editor

    void tick(LevelEditorLayer* editor) {
        if (!this->isActiveFor(editor)) return;
        // While playtesting, triggers move objects around: don't treat that as edits
        if (editor->m_playbackMode != PlaybackMode::Not) {
            status = "Paused while playtesting";
            return;
        }
        m_ticks++;

        // 1) send our own edits (one batch at a time)
        if (!m_pushing) {
            auto ops = this->scanLocal(editor, false);
            if (m_retry.size() > 0) {
                // a send failed earlier: send those changes again, plus the new ones
                auto all = std::move(m_retry);
                m_retry = matjson::Value(std::vector<matjson::Value>{});
                for (auto& op : ops) all.push(op);
                ops = std::move(all);
            }
            if (ops.size() > 0) this->push(std::move(ops));
        }
        // 2) pull in everyone else's edits (not while our own batch is on its way,
        //    so the newest change always wins the same way for everyone)
        if (!m_pushing && !m_polling) this->poll();
        // 3) who's here, and where everyone's cursor is
        if (!m_pinging) this->ping();
        // 4) progress is saved online: one person (the lowest id online) saves for everyone
        if (m_dirty && this->isSaver() && m_ticks - m_lastSaveTick >= 30 && !m_pushing && !m_saving) this->saveSnapshot(editor);
    }

private:
    WeakRef<LevelEditorLayer> m_editor;
    std::string m_client = randomString(8, "abcdefghijklmnopqrstuvwxyz0123456789");
    uint64_t m_counter = 0;
    int64_t m_lastEvent = 0;
    int64_t m_maxOwnEvent = 0;
    int m_ticks = 0;
    bool m_pushing = false;
    bool m_polling = false;
    bool m_pinging = false;
    bool m_saving = false;
    bool m_dirty = false;      // something changed since the last online save
    int m_lastSaveTick = 0;
    int64_t m_safeEvent = 0;   // saved copies include every change up to this event

    static async::TaskHolder<web::WebResponse>& finalPushTask() { static async::TaskHolder<web::WebResponse> t; return t; }
    static async::TaskHolder<web::WebResponse>& finalSaveTask() { static async::TaskHolder<web::WebResponse> t; return t; }

    bool isSaver() const {
        for (auto& m : others) if (m.client < m_client) return false;
        return true;
    }
    bool m_cursorColumns = true; // false if the database wasn't updated for cursors yet
    std::unordered_map<GameObject*, Tracked> m_tracked;
    std::unordered_map<std::string, GameObject*> m_byUuid;
    std::unordered_map<std::string, int64_t> m_ownLatest; // uuid -> id of our newest event touching it
    std::map<int, std::string> m_colors; // color channel -> its settings the last time we looked
    async::TaskHolder<web::WebResponse> m_tasks;
    async::TaskHolder<web::WebResponse> m_pushTask;
    async::TaskHolder<web::WebResponse> m_pollTask;
    async::TaskHolder<web::WebResponse> m_pingTask;
    async::TaskHolder<web::WebResponse> m_snapTask;

    void reset() {
        m_client = randomString(8, "abcdefghijklmnopqrstuvwxyz0123456789");
        active = false;
        isHost = false;
        room.clear();
        members.clear();
        others.clear();
        highlights.clear();
        feed.clear();
        hasCursor = false;
        hasEditPos = false;
        m_pinging = false;
        status = "Not in a room";
        m_editor = nullptr;
        m_tracked.clear();
        m_byUuid.clear();
        m_ownLatest.clear();
        m_colors.clear();
        m_lastEvent = 0;
        m_maxOwnEvent = 0;
        m_ticks = 0;
        m_pushing = false;
        m_polling = false;
        m_retry = matjson::Value(std::vector<matjson::Value>{});
        m_saving = false;
        m_dirty = false;
        m_lastSaveTick = 0;
        m_safeEvent = 0;
        projectName.clear();
        lastSave = "Not saved yet";
        m_tasks.cancel();
        m_pushTask.cancel();
        m_pollTask.cancel();
        m_pingTask.cancel();
        m_snapTask.cancel();
    }

    std::string newUuid() {
        return fmt::format("{}{:x}", m_client.substr(0, 5), ++m_counter);
    }

    static std::string saveString(LevelEditorLayer* editor, GameObject* obj) {
        return std::string(obj->getSaveString(editor));
    }

    // Compares the level with what we saw last time. Returns the changes as ops.
    matjson::Value scanLocal(LevelEditorLayer* editor, bool silent) {
        std::vector<matjson::Value> ops;
        std::unordered_set<GameObject*> seen;
        if (editor->m_objects) {
            for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
                if (!obj) continue;
                seen.insert(obj);
                auto str = saveString(editor, obj);
                auto it = m_tracked.find(obj);
                if (it == m_tracked.end()) {
                    auto uuid = this->newUuid();
                    m_tracked[obj] = { uuid, str, obj };
                    m_byUuid[uuid] = obj;
                    if (!silent) {
                        ops.push_back(matjson::makeObject({ { "o", "a" }, { "u", uuid }, { "s", str } }));
                        lastEditPos = obj->getPosition();
                        hasEditPos = true;
                    }
                } else if (it->second.str != str) {
                    it->second.str = str;
                    if (!silent) {
                        ops.push_back(matjson::makeObject({ { "o", "m" }, { "u", it->second.uuid }, { "s", str } }));
                        lastEditPos = obj->getPosition();
                        hasEditPos = true;
                    }
                }
            }
        }
        for (auto it = m_tracked.begin(); it != m_tracked.end();) {
            if (!seen.contains(it->first)) {
                if (!silent) ops.push_back(matjson::makeObject({ { "o", "d" }, { "u", it->second.uuid } }));
                m_byUuid.erase(it->second.uuid);
                it = m_tracked.erase(it);
            } else {
                ++it;
            }
        }
        auto colors = splitColors(levelColorString(editor));
        for (auto& [id, entry] : colors) {
            auto it = m_colors.find(id);
            if (it != m_colors.end() && it->second == entry) continue;
            m_colors[id] = entry;
            if (!silent) ops.push_back(matjson::makeObject({ { "o", "c" }, { "u", fmt::format("#c{}", id) }, { "s", entry } }));
        }
        return matjson::Value(ops); // empty array = no changes
    }

    std::string snapshotJson() {
        std::vector<matjson::Value> list;
        list.reserve(m_tracked.size());
        for (auto& [obj, t] : m_tracked) list.push_back(matjson::Value(std::vector<matjson::Value>{ t.uuid, t.str }));
        if (auto editor = m_editor.lock()) {
            list.push_back(matjson::Value(std::vector<matjson::Value>{ "#colors", levelColorString(editor) }));
        }
        return matjson::Value(list).dump(matjson::NO_INDENTATION);
    }

    void push(matjson::Value ops) {
        m_pushing = true;
        std::vector<std::string> uuids;
        for (auto& op : ops) uuids.push_back(op["u"].asString().unwrapOr(""));
        auto body = matjson::makeObject({
            { "room", room },
            { "client", m_client },
            { "author", playerName() },
            { "data", ops.dump(matjson::NO_INDENTATION) },
        });
        auto req = supa();
        req.header("Prefer", "return=representation");
        req.bodyJSON(body);
        auto count = ops.size();
        m_pushTask.spawn(req.post(fmt::format("{}/rest/v1/team_events?select=id", SUPA_URL)), [this, uuids, count, ops](web::WebResponse res) {
            m_pushing = false;
            if (!res.ok()) {
                log::warn("[teams] Send failed: HTTP {} {}", res.code(), res.string().unwrapOr("").substr(0, 200));
                status = "Connection problem, retrying...";
                // put the changes back so they're sent next time
                m_retry = ops;
                return;
            }
            auto json = res.json().unwrapOr(matjson::Value());
            int64_t id = json.isArray() && json.size() > 0 ? json[0]["id"].asInt().unwrapOr(0) : 0;
            for (auto& u : uuids) m_ownLatest[u] = id;
            m_maxOwnEvent = std::max(m_maxOwnEvent, id);
            m_dirty = true;
            status = fmt::format("Sent {} change{}", count, count == 1 ? "" : "s");
            if (m_retry.size() > 0) {
                auto retry = std::move(m_retry);
                m_retry = matjson::Value(std::vector<matjson::Value>{});
                this->push(std::move(retry));
            }
        });
    }
    matjson::Value m_retry = matjson::Value(std::vector<matjson::Value>{}); // note: matjson::Value() is an empty OBJECT, not null

    void poll() {
        m_polling = true;
        // polls only start while none of our batches are on the way, so every batch of ours
        // up to m_maxOwnEvent is in, and the poll brings everyone else's up to now
        auto ownAtStart = m_maxOwnEvent;
        auto url = fmt::format(
            "{}/rest/v1/team_events?room=eq.{}&id=gt.{}&client=neq.{}&select=id,client,author,data&order=id.asc&limit=200",
            SUPA_URL, room, m_lastEvent, m_client
        );
        m_pollTask.spawn(supa().get(url), [this, ownAtStart](web::WebResponse res) {
            m_polling = false;
            auto editor = m_editor.lock();
            if (!editor || !active) return;
            if (!res.ok()) {
                status = "Connection problem, retrying...";
                return;
            }
            auto json = res.json().unwrapOr(matjson::Value());
            if (!json.isArray()) return;
            if (editor->m_playbackMode != PlaybackMode::Not) return; // apply after playtest
            int applied = 0;
            std::string lastAuthor;
            for (auto& ev : json) {
                int64_t id = ev["id"].asInt().unwrapOr(0);
                auto ops = matjson::parse(ev["data"].asString().unwrapOr("[]")).unwrapOr(matjson::Value());
                lastAuthor = ev["author"].asString().unwrapOr("Someone");
                auto client = ev["client"].asString().unwrapOr("");
                if (ops.isArray()) applied += this->applyOps(editor, ops, id, lastAuthor, client);
                m_lastEvent = std::max(m_lastEvent, id);
            }
            if (applied > 0) m_dirty = true;
            if (json.size() < 200) m_safeEvent = std::max({ m_safeEvent, m_lastEvent, ownAtStart });
            if (applied > 0) status = fmt::format("{} edited {} object{}", lastAuthor, applied, applied == 1 ? "" : "s");
        });
    }

    // Applies someone else's changes. Newest event wins: skip anything we changed more recently.
    int applyOps(LevelEditorLayer* editor, matjson::Value const& ops, int64_t eventId, std::string const& author, std::string const& client) {
        std::vector<std::pair<std::string, std::string>> toCreate;
        std::vector<CCRect> rects;
        std::vector<std::string> colorEntries;
        int count = 0, placed = 0, edited = 0, deleted = 0;
        for (auto& op : ops) {
            auto kind = op["o"].asString().unwrapOr("");
            auto uuid = op["u"].asString().unwrapOr("");
            if (uuid.empty()) continue;
            auto own = m_ownLatest.find(uuid);
            if (own != m_ownLatest.end() && own->second > eventId) continue;

            if (kind == "c") {
                colorEntries.push_back(op["s"].asString().unwrapOr(""));
                count++;
                continue;
            }
            if (auto existing = m_byUuid.find(uuid); existing != m_byUuid.end()) {
                if (kind == "d") rects.push_back(existing->second->getObjectRect());
                this->removeTracked(editor, existing->second);
            }
            if (kind == "a" || kind == "m") toCreate.push_back({ uuid, op["s"].asString().unwrapOr("") });
            if (kind == "a") placed++;
            else if (kind == "m") edited++;
            else if (kind == "d") deleted++;
            count++;
        }
        this->createObjects(editor, toCreate);
        this->applyColors(editor, colorEntries);
        if (count == 0) return 0;

        for (auto& [uuid, str] : toCreate) {
            if (auto it = m_byUuid.find(uuid); it != m_byUuid.end()) rects.push_back(it->second->getObjectRect());
        }
        auto color = colorForClient(client);
        if (!rects.empty()) {
            if (rects.size() > 40) {
                // lots of objects: one box around all of them
                float x0 = rects[0].getMinX(), y0 = rects[0].getMinY(), x1 = rects[0].getMaxX(), y1 = rects[0].getMaxY();
                for (auto& r : rects) {
                    x0 = std::min(x0, r.getMinX()); y0 = std::min(y0, r.getMinY());
                    x1 = std::max(x1, r.getMaxX()); y1 = std::max(y1, r.getMaxY());
                }
                rects = { CCRect(x0, y0, x1 - x0, y1 - y0) };
            }
            highlights.push_back({ std::move(rects), author, color });
            if (highlights.size() > 20) highlights.erase(highlights.begin());
        }
        std::string what;
        auto add = [&](int n, char const* verb) {
            if (n > 0) what += fmt::format("{}{} {}", what.empty() ? "" : ", ", verb, n);
        };
        add(placed, "placed");
        add(edited, "edited");
        add(deleted, "deleted");
        if (!colorEntries.empty()) {
            what += fmt::format("{}changed {} color{}", what.empty() ? "" : ", ", colorEntries.size(), colorEntries.size() == 1 ? "" : "s");
        }
        feed.push_back({ fmt::format("{} {}", author, what), color, 0.f });
        while (feed.size() > 4) feed.pop_front();
        return count;
    }

    // Sets color channels from their saved form (same format GD saves levels in)
    void applyColors(LevelEditorLayer* editor, std::vector<std::string> const& entries) {
        if (entries.empty() || !editor->m_levelSettings || !editor->m_levelSettings->m_effectManager) return;
        auto settingsColors = editor->m_levelSettings->m_effectManager;
        for (auto& entry : entries) {
            int id = colorEntryChannel(entry);
            if (id <= 0) continue;
            auto action = settingsColors->getColorAction(id);
            if (!action) continue;
            action->setupFromString(entry);
            action->m_color = action->m_fromColor;
            action->m_currentOpacity = action->m_fromOpacity;
        }
        // copy the level's colors into the live editor so they show right away
        if (editor->m_effectManager) {
            editor->transferDefaultColors(settingsColors, editor->m_effectManager);
            editor->m_effectManager->calculateBaseActiveColors();
            editor->m_effectManager->processInheritedColors();
        }
        // remember how GD saves them now, so we don't send them straight back
        auto now = splitColors(levelColorString(editor));
        for (auto& entry : entries) {
            int id = colorEntryChannel(entry);
            if (auto it = now.find(id); it != now.end()) m_colors[id] = it->second;
        }
    }

    void removeTracked(LevelEditorLayer* editor, GameObject* obj) {
        Ref<GameObject> hold = obj; // stay valid while we remove it
        if (auto it = m_tracked.find(obj); it != m_tracked.end()) {
            m_byUuid.erase(it->second.uuid);
            m_tracked.erase(it);
        }
        // the player may have deleted it already
        if (!obj->getParent()) return;
        if (obj->m_isSelected && editor->m_editorUI) editor->m_editorUI->deselectObject(obj);
        editor->removeObject(obj, true);
    }

    void track(LevelEditorLayer* editor, GameObject* obj, std::string const& uuid) {
        m_tracked[obj] = { uuid, saveString(editor, obj), obj };
        m_byUuid[uuid] = obj;
    }

    void createObjects(LevelEditorLayer* editor, std::vector<std::pair<std::string, std::string>> const& objects) {
        std::vector<std::pair<std::string, std::string>> list;
        for (auto& o : objects) if (!o.second.empty()) list.push_back(o);
        if (list.empty()) return;

        // Fast path: create everything in one go (big levels would freeze the game otherwise)
        if (list.size() > 1) {
            std::string all;
            for (auto& [uuid, str] : list) {
                all += str;
                if (!str.empty() && str.back() != ';') all += ';';
            }
            auto created = editor->createObjectsFromString(all, true, true);
            if (created && created->count() == list.size()) {
                for (size_t i = 0; i < list.size(); i++) {
                    this->track(editor, static_cast<GameObject*>(created->objectAtIndex(i)), list[i].first);
                }
                return;
            }
            // counts didn't line up: undo and fall back to one by one
            log::warn("[teams] Batch create made {} of {} objects, creating one by one", created ? created->count() : 0, list.size());
            if (created) {
                for (auto obj : CCArrayExt<GameObject*>(created)) editor->removeObject(obj, true);
            }
        }
        for (auto& [uuid, str] : list) {
            auto created = editor->createObjectsFromString(str, true, true);
            if (!created || created->count() == 0) continue;
            this->track(editor, static_cast<GameObject*>(created->objectAtIndex(0)), uuid);
        }
    }

    void clearLevel(LevelEditorLayer* editor) {
        m_tracked.clear();
        m_byUuid.clear();
        // GD's own "delete everything": fast, and also clears undo/redo so nothing points at deleted objects
        editor->removeAllObjects();
    }

    void ping() {
        m_pinging = true;
        auto body = matjson::makeObject({
            { "room", room }, { "client", m_client }, { "name", playerName() }, { "last_seen", isoNow() },
        });
        if (m_cursorColumns) {
            body.set("cursor_x", std::round(cursor.x * 10.f) / 10.f);
            body.set("cursor_y", std::round(cursor.y * 10.f) / 10.f);
            body.set("has_cursor", hasCursor);
        }
        auto req = supa();
        req.header("Prefer", "resolution=merge-duplicates,return=minimal");
        req.bodyJSON(body);
        auto roomCode = room;
        m_pingTask.spawn(req.post(fmt::format("{}/rest/v1/team_members?on_conflict=room,client", SUPA_URL)), [this, roomCode](web::WebResponse res) {
            if (!active || room != roomCode) { m_pinging = false; return; }
            if (!res.ok() && m_cursorColumns && res.string().unwrapOr("").find("cursor") != std::string::npos) {
                // the cursor columns don't exist: keep working without cursors
                log::warn("[teams] Database has no cursor columns yet, run sql/update-cursors.sql");
                m_cursorColumns = false;
                m_pinging = false;
                return;
            }
            auto url = fmt::format(
                "{}/rest/v1/team_members?room=eq.{}&last_seen=gt.{}&select={}&order=name.asc",
                SUPA_URL, roomCode, isoNow(-20), m_cursorColumns ? "client,name,cursor_x,cursor_y,has_cursor,kicked" : "client,name"
            );
            m_pingTask.spawn(supa().get(url), [this, roomCode](web::WebResponse res) {
                m_pinging = false;
                if (!active || room != roomCode || !res.ok()) return;
                auto json = res.json().unwrapOr(matjson::Value());
                if (!json.isArray()) return;
                members.clear();
                others.clear();
                for (auto& m : json) {
                    auto name = m["name"].asString().unwrapOr("?");
                    auto client = m["client"].asString().unwrapOr("");
                    if (m["kicked"].asBool().unwrapOr(false)) {
                        if (client == m_client) {
                            // the admin kicked (or banned) us
                            log::info("[teams] Kicked from {}", room);
                            Loader::get()->queueInMainThread([this, roomCode] { if (room == roomCode) this->reset(); });
                            Notification::create("The admin removed you from this team level", NotificationIcon::Error, 4.f)->show();
                            return;
                        }
                        continue;
                    }
                    members.push_back(name);
                    if (client.empty() || client == m_client) continue;
                    Member member;
                    member.client = client;
                    member.name = name;
                    member.x = static_cast<float>(m["cursor_x"].asDouble().unwrapOr(0.0));
                    member.y = static_cast<float>(m["cursor_y"].asDouble().unwrapOr(0.0));
                    member.hasCursor = m["has_cursor"].asBool().unwrapOr(false);
                    member.color = colorForClient(client);
                    others.push_back(std::move(member));
                }
            });
        });
    }

    void saveSnapshot(LevelEditorLayer* editor) {
        // make sure everything we have is already sent and tracked
        auto ops = this->scanLocal(editor, false);
        if (ops.size() > 0) return this->push(std::move(ops)); // try again next time
        auto [req, url] = this->snapshotRequest();
        m_saving = true;
        m_lastSaveTick = m_ticks;
        auto roomCode = room;
        m_snapTask.spawn(req.patch(url), [this, roomCode](web::WebResponse res) {
            if (room != roomCode) return;
            m_saving = false;
            this->onSaved(res);
        });
    }

    std::pair<web::WebRequest, std::string> snapshotRequest() {
        auto body = matjson::makeObject({
            { "snapshot", this->snapshotJson() },
            { "snapshot_event", m_safeEvent },
            { "saved_by", playerName() },
            { "objects", static_cast<int>(m_tracked.size()) },
            { "updated_at", isoNow() },
        });
        auto req = supa();
        req.header("Prefer", "return=representation");
        req.bodyJSON(body);
        // never replace a newer saved copy with an older one
        auto url = fmt::format("{}/rest/v1/team_projects?code=eq.{}&snapshot_event=lte.{}&select=code", SUPA_URL, room, m_safeEvent);
        return { std::move(req), url };
    }

    void onSaved(web::WebResponse& res) {
        if (!res.ok()) {
            log::warn("[teams] Couldn't save: HTTP {}", res.code());
            lastSave = "Save failed, retrying soon";
            return;
        }
        m_dirty = false;
        auto json = res.json().unwrapOr(matjson::Value());
        // empty answer = someone already saved a newer copy, which is just as good
        lastSave = "Saved online just now";
        m_savedAt = std::chrono::steady_clock::now();
    }

public:
    std::string saveText() const {
        if (lastSave != "Saved online just now") return lastSave;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_savedAt).count();
        if (secs < 10) return "Saved online just now";
        if (secs < 60) return fmt::format("Saved {}s ago", secs);
        return fmt::format("Saved {}m ago", secs / 60);
    }

private:
    std::chrono::steady_clock::time_point m_savedAt;
};

// ---------------------------------------------------------------------------
// Editor hooks: a small node that ticks the session and shows the status
// ---------------------------------------------------------------------------

// Set by the Team Gaming screen: what to do once the level's editor opens
enum class PendingAction { None, Host, Join };
static PendingAction s_pendingAction = PendingAction::None;
static std::string s_pendingCode;
static std::string s_pendingName;

// Last spot touched in the editor (phones have no mouse)
static CCPoint s_lastTouch = { 0.f, 0.f };
static bool s_touchValid = false;

class CollabTicker : public CCNode {
public:
    static constexpr float FLASH_TIME = 2.5f;
    static constexpr float FEED_TIME = 12.f;

    WeakRef<LevelEditorLayer> m_editor;
    CCLabelBMFont* m_label = nullptr;
    CCLabelBMFont* m_feedLabels[4] = {};
    float m_elapsed = 0.f;
    float m_startDelay = 0.f;

    // cursors and flashes live inside the editor's object layer, so they move and zoom with the level
    Ref<CCNode> m_overlay;
    CCDrawNode* m_flashDraw = nullptr;

    struct CursorNode {
        Ref<CCNode> node;
        CCPoint pos;
        CCPoint target;
    };
    std::unordered_map<std::string, CursorNode> m_cursors;

    struct Flash {
        std::vector<CCRect> rects;
        ccColor3B color;
        float life = FLASH_TIME;
        Ref<CCLabelBMFont> tag;
    };
    std::vector<Flash> m_flashes;

    static CollabTicker* create(LevelEditorLayer* editor) {
        auto ret = new CollabTicker();
        ret->m_editor = editor;
        ret->init();
        ret->autorelease();
        return ret;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        m_label = CCLabelBMFont::create("", "chatFont.fnt");
        m_label->setAnchorPoint({ 0.f, 1.f });
        m_label->setScale(0.55f);
        m_label->setPosition({ 75.f, winSize.height - 60.f });
        m_label->setOpacity(220);
        this->addChild(m_label);
        for (int i = 0; i < 4; i++) {
            auto lbl = CCLabelBMFont::create("", "chatFont.fnt");
            lbl->setAnchorPoint({ 0.f, 1.f });
            lbl->setScale(0.5f);
            lbl->setPosition({ 75.f, winSize.height - 74.f - i * 12.f });
            this->addChild(lbl);
            m_feedLabels[i] = lbl;
        }
        s_touchValid = false;
        this->scheduleUpdate();
        return true;
    }

    void runPendingAction(LevelEditorLayer* editor) {
        auto action = s_pendingAction;
        auto code = s_pendingCode;
        auto name = s_pendingName;
        s_pendingAction = PendingAction::None;
        s_pendingCode.clear();
        s_pendingName.clear();
        if (action == PendingAction::Join) {
            Notification::create(fmt::format("Loading {}...", name), NotificationIcon::Loading, 1.f)->show();
            CollabSession::get().join(editor, code, [](bool ok, std::string err) {
                if (!ok) Notification::create(err, NotificationIcon::Error, 3.f)->show();
                else Notification::create("Loaded! Your team's edits sync live", NotificationIcon::Success)->show();
            });
        } else if (action == PendingAction::Host) {
            Notification::create("Creating team level...", NotificationIcon::Loading, 1.f)->show();
            CollabSession::get().host(editor, name, [](bool ok, std::string err) {
                if (!ok) Notification::create(err, NotificationIcon::Error, 3.f)->show();
                else Notification::create("Team level created! Your team can open it now", NotificationIcon::Success, 3.f)->show();
            });
        }
    }

    void update(float dt) override {
        auto editor = m_editor.lock();
        if (!editor) return;
        auto& s = CollabSession::get();

        if (s_pendingAction != PendingAction::None) {
            m_startDelay += dt;
            if (m_startDelay > 0.6f) this->runPendingAction(editor);
        }

        bool mine = s.isActiveFor(editor);
        m_label->setVisible(mine);
        if (!mine) {
            for (auto lbl : m_feedLabels) lbl->setVisible(false);
            if (m_overlay) m_overlay->setVisible(false);
            return;
        }

        this->updateLocalCursor(editor);

        m_elapsed += dt;
        float interval = static_cast<float>(Mod::get()->getSettingValue<double>("sync-interval"));
        if (m_elapsed >= interval) {
            m_elapsed = 0.f;
            s.tick(editor);
        }
        m_label->setString(fmt::format("{}  |  {} online  |  {}  |  {}", s.projectName, std::max<size_t>(1, s.members.size()), s.saveText(), s.status).c_str());

        this->updateFeed(dt);
        if (this->ensureOverlay(editor)) {
            m_overlay->setVisible(true);
            float zoom = std::max(0.05f, editor->m_objectLayer->getScale());
            this->updateCursors(dt, zoom);
            this->updateFlashes(dt, zoom);
        }
    }

    void updateLocalCursor(LevelEditorLayer* editor) {
        auto& s = CollabSession::get();
        if (!editor->m_objectLayer) return;
#ifdef GEODE_IS_DESKTOP
        // PC: show where we last placed or moved a block
        if (!s.hasEditPos) return;
        s.cursor = s.lastEditPos;
        s.hasCursor = true;
#else
        if (!s_touchValid) return;
        s.cursor = editor->m_objectLayer->convertToNodeSpace(s_lastTouch);
        s.hasCursor = true;
#endif
    }

    void updateFeed(float dt) {
        auto& feed = CollabSession::get().feed;
        for (auto& line : feed) line.age += dt;
        while (!feed.empty() && feed.front().age > FEED_TIME) feed.pop_front();
        for (size_t i = 0; i < 4; i++) {
            auto lbl = m_feedLabels[i];
            if (i >= feed.size()) {
                lbl->setVisible(false);
                continue;
            }
            // newest line on top
            auto& line = feed[feed.size() - 1 - i];
            lbl->setVisible(true);
            lbl->setString(line.text.c_str());
            lbl->setColor(line.color);
            float left = FEED_TIME - line.age;
            lbl->setOpacity(static_cast<GLubyte>(230.f * std::clamp(left / 2.f, 0.f, 1.f)));
        }
    }

    bool ensureOverlay(LevelEditorLayer* editor) {
        auto layer = editor->m_objectLayer;
        if (!layer) return false;
        if (!m_overlay) {
            m_overlay = CCNode::create();
            m_overlay->setID("collab-overlay"_spr);
            m_flashDraw = CCDrawNode::create();
            m_overlay->addChild(m_flashDraw);
        }
        if (m_overlay->getParent() != layer) {
            m_overlay->removeFromParent();
            layer->addChild(m_overlay, 9999);
        }
        return true;
    }

    static CCNode* makeCursor(std::string const& name, ccColor3B color) {
        auto node = CCNode::create();
        auto draw = CCDrawNode::create();
        CCPoint arrow[] = { { 0.f, 0.f }, { 0.f, -20.f }, { 5.f, -15.f }, { 9.f, -23.f }, { 12.5f, -21.5f }, { 8.5f, -14.f }, { 15.f, -14.f } };
        draw->drawPolygon(arrow, 7, ccc4FFromccc3B(color), 1.2f, { 0.f, 0.f, 0.f, 1.f });
        node->addChild(draw);

        auto lbl = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
        lbl->setScale(0.5f);
        lbl->limitLabelWidth(110.f, 0.5f, 0.2f);
        lbl->setColor(color);
        lbl->setAnchorPoint({ 0.f, 0.5f });
        auto size = lbl->getScaledContentSize();
        auto bg = CCLayerColor::create({ 0, 0, 0, 170 }, size.width + 8.f, size.height + 2.f);
        bg->setPosition({ 12.f, -30.f - size.height / 2.f - 1.f });
        lbl->setPosition({ 16.f, -30.f });
        node->addChild(bg);
        node->addChild(lbl);
        return node;
    }

    void updateCursors(float dt, float zoom) {
        auto& s = CollabSession::get();
        std::unordered_set<std::string> seen;
        for (auto& m : s.others) {
            if (!m.hasCursor) continue;
            seen.insert(m.client);
            CCPoint target = { m.x, m.y };
            auto it = m_cursors.find(m.client);
            if (it == m_cursors.end()) {
                auto node = makeCursor(m.name, m.color);
                node->setPosition(target);
                m_overlay->addChild(node, 10);
                m_cursors[m.client] = { node, target, target };
            } else {
                it->second.target = target;
            }
        }
        for (auto it = m_cursors.begin(); it != m_cursors.end();) {
            if (!seen.contains(it->first)) {
                it->second.node->removeFromParent();
                it = m_cursors.erase(it);
                continue;
            }
            auto& c = it->second;
            // glide toward the latest position so cursors don't jump once a second
            float t = std::min(1.f, dt * 8.f);
            c.pos = c.pos + (c.target - c.pos) * t;
            c.node->setPosition(c.pos);
            c.node->setScale(1.f / zoom);
            ++it;
        }
    }

    void updateFlashes(float dt, float zoom) {
        auto& s = CollabSession::get();
        for (auto& h : s.highlights) {
            Flash f;
            f.rects = std::move(h.rects);
            f.color = h.color;
            float x0 = f.rects[0].getMinX(), y1 = f.rects[0].getMaxY();
            for (auto& r : f.rects) { x0 = std::min(x0, r.getMinX()); y1 = std::max(y1, r.getMaxY()); }
            auto tag = CCLabelBMFont::create(h.name.c_str(), "chatFont.fnt");
            tag->setColor(h.color);
            tag->setAnchorPoint({ 0.f, 0.f });
            tag->setPosition({ x0, y1 + 2.f });
            m_overlay->addChild(tag, 5);
            f.tag = tag;
            m_flashes.push_back(std::move(f));
        }
        s.highlights.clear();

        m_flashDraw->clear();
        for (auto it = m_flashes.begin(); it != m_flashes.end();) {
            it->life -= dt;
            if (it->life <= 0.f) {
                it->tag->removeFromParent();
                it = m_flashes.erase(it);
                continue;
            }
            float a = it->life / FLASH_TIME;
            auto c = ccc4FFromccc3B(it->color);
            ccColor4F fill = { c.r, c.g, c.b, 0.3f * a };
            ccColor4F line = { c.r, c.g, c.b, a };
            for (auto& r : it->rects) {
                CCPoint box[] = {
                    { r.getMinX(), r.getMinY() }, { r.getMaxX(), r.getMinY() },
                    { r.getMaxX(), r.getMaxY() }, { r.getMinX(), r.getMaxY() },
                };
                m_flashDraw->drawPolygon(box, 4, fill, 1.5f / zoom, line);
            }
            it->tag->setScale(0.5f / zoom);
            it->tag->setOpacity(static_cast<GLubyte>(255.f * a));
            ++it;
        }
    }

    void onExit() override {
        // leaving the editor (exit, save & play...) ends the session
        // GD is switching scenes right now, so don't do any work here: save and
        // leave on the next frame instead (keeping the editor alive until then)
        auto editor = m_editor.lock();
        log::info("[teams] Editor closing (in a team level: {})", editor && CollabSession::get().isActiveFor(editor));
        if (editor && CollabSession::get().isActiveFor(editor)) {
            Ref<LevelEditorLayer> keep = editor;
            Loader::get()->queueInMainThread([keep] {
                auto& s = CollabSession::get();
                if (!s.isActiveFor(keep)) return;
                log::info("[teams] Saving after leaving the editor");
                s.saveNow(keep, true); // keep the team's progress
                s.leave();
                log::info("[teams] Saved and left");
            });
        }
        m_cursors.clear();
        m_flashes.clear();
        CCNode::onExit();
    }
};

#ifndef GEODE_IS_DESKTOP
class $modify(CollabEditorUI, EditorUI) {
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) {
        s_lastTouch = touch->getLocation();
        s_touchValid = true;
        return EditorUI::ccTouchBegan(touch, event);
    }
    void ccTouchMoved(CCTouch* touch, CCEvent* event) {
        s_lastTouch = touch->getLocation();
        EditorUI::ccTouchMoved(touch, event);
    }
};
#endif

class $modify(CollabEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        auto ticker = CollabTicker::create(this);
        ticker->setID("collab-ticker"_spr);
        this->addChild(ticker, 1000);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Who's on the team: data/members.txt in the repo (edit it, no mod update needed)
// ---------------------------------------------------------------------------

static constexpr auto MEMBERS_URL = "https://raw.githubusercontent.com/pester33/Teams-yay/main/data/members.txt";
static async::TaskHolder<web::WebResponse> s_membersTask;
static bool s_membersLoaded = false;
static bool s_everyone = true;
static std::unordered_set<std::string> s_teamNames;

static std::string lower(std::string str) {
    for (auto& c : str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return str;
}

static void loadTeamMembers(std::function<void()> done = nullptr) {
    web::WebRequest req;
    setTimeout(req, 15);
    s_membersTask.spawn(req.get(MEMBERS_URL), [done](web::WebResponse res) {
        if (res.ok()) {
            s_teamNames.clear();
            s_everyone = false;
            for (auto line : utils::string::split(res.string().unwrapOr(""), "\n")) {
                line = utils::string::trim(line);
                if (line.empty() || line[0] == '#') continue;
                if (line == "*") s_everyone = true;
                else s_teamNames.insert(lower(line));
            }
            s_membersLoaded = true;
        }
        // if the list can't be loaded, don't lock anyone out
        if (done) done();
    });
}

static bool isOnTeam() {
    if (!s_membersLoaded || s_everyone) return true;
    return s_teamNames.contains(lower(playerName()));
}

// "2026-11-26T15:08:55.123+00:00" -> "5m ago"
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

static std::string timeAgo(std::string const& iso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (iso.size() < 19 || std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return "";
    int64_t then = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t diff = std::max<int64_t>(0, now - then);
    if (diff < 60) return "just now";
    if (diff < 3600) return fmt::format("{}m ago", diff / 60);
    if (diff < 86400) return fmt::format("{}h ago", diff / 3600);
    return fmt::format("{}d ago", diff / 86400);
}

// ---------------------------------------------------------------------------
// Opening a team level: reuse its copy in My Levels, or make one
// ---------------------------------------------------------------------------

static void openTeamLevel(PendingAction action, std::string const& code, std::string const& name) {
    if (!isOnTeam()) {
        Notification::create("Only Gaming team members can open team levels", NotificationIcon::Warning, 3.f)->show();
        return;
    }
    // team levels are named "TG <name>" in My Levels, so they never mix with your own levels
    std::string localName = fmt::format("TG {}", name).substr(0, 30);
    GJGameLevel* level = nullptr;
    if (action == PendingAction::Join) {
        if (auto levels = LocalLevelManager::get()->m_localLevels) {
            for (auto lvl : CCArrayExt<GJGameLevel*>(levels)) {
                if (lvl && std::string(lvl->m_levelName) == localName) { level = lvl; break; }
            }
        }
    }
    if (!level) level = GameLevelManager::get()->createNewLevel();
    if (!level) {
        Notification::create("Couldn't create a level", NotificationIcon::Error)->show();
        return;
    }
    level->m_levelName = localName;
    s_pendingAction = action;
    s_pendingCode = code;
    s_pendingName = name;
    // Open it the same way GD does for your own levels: the level's page first, then Edit.
    // (Jumping straight into the editor made GD's "Save and Play" load forever.)
    auto scene = EditLevelLayer::scene(level);
    if (auto page = scene->getChildByType<EditLevelLayer>(0)) {
        // press Edit once the page is really on screen (actions only run then)
        page->runAction(CCSequence::create(
            CCDelayTime::create(0.15f),
            CallFuncExt::create([page] { page->onEdit(nullptr); }),
            nullptr
        ));
    }
    CCDirector::sharedDirector()->replaceScene(scene);
}

// small helpers shared by the popups
static CCMenuItemSpriteExtra* textButton(char const* text, char const* texture, float scale, std::function<void()> fn) {
    auto spr = ButtonSprite::create(text, "goldFont.fnt", texture, 0.8f);
    spr->setScale(scale);
    return CCMenuItemExt::createSpriteExtra(spr, [fn](CCMenuItemSpriteExtra*) { fn(); });
}

static CCLabelBMFont* addText(CCNode* parent, std::string const& str, char const* font, float scale, CCPoint pos, ccColor3B color = { 255, 255, 255 }) {
    auto lbl = CCLabelBMFont::create(str.c_str(), font);
    lbl->setScale(scale);
    lbl->setColor(color);
    lbl->setPosition(pos);
    parent->addChild(lbl);
    return lbl;
}

// ---------------------------------------------------------------------------
// New team level: pick a name
// ---------------------------------------------------------------------------

class NewTeamLevelPopup : public Popup {
protected:
    static constexpr float W = 300.f;
    static constexpr float H = 170.f;
    TextInput* m_name = nullptr;

    bool init() {
        if (!Popup::init(W, H)) return false;
        this->setTitle("New Team Level");
        addText(m_mainLayer, "Name it so your team knows what it is:", "chatFont.fnt", 0.55f, { W / 2, H - 50.f });
        m_name = TextInput::create(220.f, "Level name", "bigFont.fnt");
        m_name->setFilter("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_!?.'&");
        m_name->setMaxCharCount(24);
        m_name->setPosition({ W / 2, H - 85.f });
        m_mainLayer->addChild(m_name);
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(menu);
        auto create = textButton("Create", "GJ_button_01.png", 0.75f, [this] {
            std::string name = utils::string::trim(std::string(m_name->getString()));
            if (name.empty()) {
                Notification::create("Give the level a name first", NotificationIcon::Warning)->show();
                return;
            }
            this->onClose(nullptr);
            openTeamLevel(PendingAction::Host, "", name);
        });
        create->setPosition({ W / 2, 32.f });
        menu->addChild(create);
        return true;
    }

public:
    static NewTeamLevelPopup* create() {
        auto ret = new NewTeamLevelPopup();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Admin: the person who made a team level has its admin key on their device
// ---------------------------------------------------------------------------

static std::string ownerKeyFor(std::string const& code) {
    return Mod::get()->getSavedValue<std::string>("owner-" + code, "");
}

// Calls a database function (checked on the server, so only the real admin can use admin actions)
static void callRpc(async::TaskHolder<web::WebResponse>& holder, char const* fn, matjson::Value body, std::function<void(bool, std::string)> done) {
    auto req = supa();
    req.bodyJSON(body);
    holder.spawn(req.post(fmt::format("{}/rest/v1/rpc/{}", SUPA_URL, fn)), [done](web::WebResponse res) {
        if (!res.ok()) {
            log::warn("[teams] rpc failed: HTTP {} {}", res.code(), res.string().unwrapOr("").substr(0, 200));
            return done(false, fmt::format("HTTP {}", res.code()));
        }
        auto json = res.json().unwrapOr(matjson::Value());
        done(true, json.isString() ? json.asString().unwrapOr("") : json.dump(matjson::NO_INDENTATION));
    });
}

static void tryOpenProject(std::string code, std::string name, std::string joinCode = "");

class JoinCodePopup : public Popup {
protected:
    static constexpr float W = 280.f;
    static constexpr float H = 160.f;
    std::string m_code, m_name;
    TextInput* m_input = nullptr;

    bool init(std::string code, std::string name) {
        if (!Popup::init(W, H)) return false;
        m_code = code;
        m_name = name;
        this->setTitle("Join Code");
        addText(m_mainLayer, fmt::format("\"{}\" needs a join code:", name), "chatFont.fnt", 0.55f, { W / 2, H - 50.f })
            ->limitLabelWidth(W - 30.f, 0.55f, 0.2f);
        m_input = TextInput::create(180.f, "Code", "bigFont.fnt");
        m_input->setMaxCharCount(20);
        m_input->setPosition({ W / 2, H - 82.f });
        m_mainLayer->addChild(m_input);
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(menu);
        auto join = textButton("Join", "GJ_button_02.png", 0.7f, [this] {
            std::string entered = m_input->getString();
            auto code = m_code, name = m_name;
            this->onClose(nullptr);
            tryOpenProject(code, name, entered);
        });
        join->setPosition({ W / 2, 30.f });
        menu->addChild(join);
        return true;
    }

public:
    static JoinCodePopup* create(std::string code, std::string name) {
        auto ret = new JoinCodePopup();
        if (ret->init(code, name)) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

static async::TaskHolder<web::WebResponse> s_checkTask;

// Asks the server if we may open this level (not banned, right join code), then opens it
static void tryOpenProject(std::string code, std::string name, std::string joinCode) {
    if (!isOnTeam()) {
        Notification::create("Only Gaming team members can open team levels", NotificationIcon::Warning, 3.f)->show();
        return;
    }
    if (!ownerKeyFor(code).empty()) return openTeamLevel(PendingAction::Join, code, name); // the admin
    if (joinCode.empty()) joinCode = Mod::get()->getSavedValue<std::string>("joincode-" + code, "");
    Notification::create("Checking...", NotificationIcon::Loading, 0.6f)->show();
    callRpc(s_checkTask, "team_check", matjson::makeObject({
        { "p_code", code }, { "p_name", playerName() }, { "p_join_code", joinCode },
    }), [code, name, joinCode](bool ok, std::string result) {
        if (!ok) {
            // the check isn't set up on the database yet: don't block the team
            return openTeamLevel(PendingAction::Join, code, name);
        }
        if (result == "ok") {
            if (!joinCode.empty()) Mod::get()->setSavedValue<std::string>("joincode-" + code, joinCode);
            return openTeamLevel(PendingAction::Join, code, name);
        }
        if (result == "banned") {
            Notification::create("You're banned from this team level", NotificationIcon::Error, 3.f)->show();
        } else if (result == "code" || result == "wrong") {
            if (result == "wrong") Notification::create("Wrong join code", NotificationIcon::Error, 2.f)->show();
            Mod::get()->setSavedValue<std::string>("joincode-" + code, "");
            if (auto p = JoinCodePopup::create(code, name)) p->show();
        } else if (result == "missing") {
            Notification::create("That team level was deleted", NotificationIcon::Warning, 3.f)->show();
        } else {
            Notification::create(fmt::format("Can't open: {}", result), NotificationIcon::Error, 3.f)->show();
        }
    });
}

class TeamAdminPopup : public Popup {
protected:
    static constexpr float W = 400.f;
    static constexpr float H = 285.f;
    std::string m_code;
    std::string m_key;
    std::string m_joinCode;
    std::vector<std::string> m_banned;
    bool m_infoLoaded = false;
    CCNode* m_content = nullptr;
    TextInput* m_nameInput = nullptr;
    TextInput* m_codeInput = nullptr;
    async::TaskHolder<web::WebResponse> m_infoTask;
    async::TaskHolder<web::WebResponse> m_actionTask;

    bool init(std::string code) {
        if (!Popup::init(W, H)) return false;
        m_code = code;
        m_key = ownerKeyFor(code);
        this->setTitle("Admin Panel");

        // these two keep what you typed when the list below refreshes
        addText(m_mainLayer, "Level name", "goldFont.fnt", 0.5f, { 60.f, H - 55.f });
        m_nameInput = TextInput::create(170.f, "Name", "bigFont.fnt");
        m_nameInput->setFilter("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_!?.'&");
        m_nameInput->setMaxCharCount(24);
        m_nameInput->setString(CollabSession::get().projectName);
        m_nameInput->setPosition({ 200.f, H - 55.f });
        m_mainLayer->addChild(m_nameInput);

        addText(m_mainLayer, "Join code", "goldFont.fnt", 0.5f, { 60.f, H - 88.f });
        m_codeInput = TextInput::create(120.f, "none", "bigFont.fnt");
        m_codeInput->setMaxCharCount(20);
        m_codeInput->setPosition({ 175.f, H - 88.f });
        m_mainLayer->addChild(m_codeInput);

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(menu);
        auto rename = textButton("Rename", "GJ_button_01.png", 0.5f, [this] {
            std::string name = utils::string::trim(std::string(m_nameInput->getString()));
            if (name.empty()) return;
            this->action("rename", name, [name](std::string) {
                CollabSession::get().projectName = name;
                Notification::create("Level renamed", NotificationIcon::Success)->show();
            });
        });
        rename->setPosition({ 335.f, H - 55.f });
        menu->addChild(rename);
        auto setCode = textButton("Set", "GJ_button_01.png", 0.5f, [this] {
            std::string code = m_codeInput->getString();
            if (code.empty()) return;
            this->action("set_code", code, [this, code](std::string) {
                m_joinCode = code;
                Notification::create("Join code set: people need it to open this level", NotificationIcon::Success, 3.f)->show();
            });
        });
        setCode->setPosition({ 275.f, H - 88.f });
        menu->addChild(setCode);
        auto clearCode = textButton("Remove", "GJ_button_06.png", 0.5f, [this] {
            this->action("set_code", "", [this](std::string) {
                m_joinCode.clear();
                m_codeInput->setString("");
                Notification::create("Join code removed", NotificationIcon::Success)->show();
            });
        });
        clearCode->setPosition({ 340.f, H - 88.f });
        menu->addChild(clearCode);

        auto del = textButton("Delete level", "GJ_button_06.png", 0.5f, [this] {
            auto self = WeakRef<TeamAdminPopup>(this);
            createQuickPopup("Delete team level",
                "Delete this team level <cr>for everyone</c>? The saved progress is gone forever. (Your copy in My Levels stays.)",
                "Cancel", "Delete", [self](auto, bool yes) {
                    auto popup = self.lock();
                    if (!yes || !popup) return;
                    popup->action("delete", "", [self](std::string) {
                        CollabSession::get().leave();
                        Notification::create("Team level deleted", NotificationIcon::Success)->show();
                        if (auto p = self.lock()) p->onClose(nullptr);
                    });
                });
        });
        del->setPosition({ W - 55.f, 22.f });
        menu->addChild(del);

        this->rebuild();
        this->loadInfo();
        this->schedule(schedule_selector(TeamAdminPopup::refresh), 2.f);
        return true;
    }

    void refresh(float) { this->rebuild(); }

    void loadInfo() {
        auto self = WeakRef<TeamAdminPopup>(this);
        callRpc(m_infoTask, "team_admin", matjson::makeObject({
            { "p_code", m_code }, { "p_key", m_key }, { "p_action", "info" }, { "p_value", "" },
        }), [self](bool ok, std::string result) {
            auto popup = self.lock();
            if (!popup) return;
            if (!ok) {
                Notification::create("Admin isn't set up on the database yet (run sql/admin.sql)", NotificationIcon::Error, 4.f)->show();
                return;
            }
            if (result == "denied") {
                Notification::create("You're not the admin of this level", NotificationIcon::Error, 3.f)->show();
                return;
            }
            auto json = matjson::parse(result).unwrapOr(matjson::Value());
            popup->m_joinCode = json["code"].asString().unwrapOr("");
            popup->m_banned.clear();
            if (json["banned"].isArray()) for (auto& b : json["banned"]) popup->m_banned.push_back(b.asString().unwrapOr(""));
            if (!popup->m_infoLoaded) popup->m_codeInput->setString(popup->m_joinCode);
            popup->m_infoLoaded = true;
            popup->rebuild();
        });
    }

    void action(std::string act, std::string value, std::function<void(std::string)> onOk) {
        auto self = WeakRef<TeamAdminPopup>(this);
        callRpc(m_actionTask, "team_admin", matjson::makeObject({
            { "p_code", m_code }, { "p_key", m_key }, { "p_action", act }, { "p_value", value },
        }), [self, onOk](bool ok, std::string result) {
            if (!ok || result != "ok") {
                Notification::create(fmt::format("Didn't work: {}", ok ? result : "couldn't reach the server"), NotificationIcon::Error, 3.f)->show();
                return;
            }
            onOk(result);
            if (auto popup = self.lock()) popup->loadInfo();
        });
    }

    void rebuild() {
        if (m_content) m_content->removeFromParent();
        m_content = CCNode::create();
        m_mainLayer->addChild(m_content);
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_content->addChild(menu);
        auto& s = CollabSession::get();

        // people here right now
        addText(m_content, "Here now", "goldFont.fnt", 0.5f, { 100.f, H - 120.f });
        float y = H - 140.f;
        int shown = 0;
        for (auto& m : s.others) {
            if (shown >= 5) break;
            auto lbl = addText(m_content, m.name, "chatFont.fnt", 0.5f, { 20.f, y }, m.color);
            lbl->setAnchorPoint({ 0.f, 0.5f });
            lbl->limitLabelWidth(80.f, 0.5f, 0.2f);
            auto client = m.client, name = m.name;
            auto kick = textButton("Kick", "GJ_button_04.png", 0.4f, [this, client, name] {
                this->action("kick", client, [name](std::string) {
                    Notification::create(fmt::format("Kicked {}", name), NotificationIcon::Success)->show();
                });
            });
            kick->setPosition({ 130.f, y });
            menu->addChild(kick);
            auto ban = textButton("Ban", "GJ_button_06.png", 0.4f, [this, name] {
                this->action("ban", name, [name](std::string) {
                    Notification::create(fmt::format("Banned {}", name), NotificationIcon::Success)->show();
                });
            });
            ban->setPosition({ 175.f, y });
            menu->addChild(ban);
            y -= 22.f;
            shown++;
        }
        if (shown == 0) addText(m_content, "Nobody else right now", "chatFont.fnt", 0.45f, { 100.f, y }, { 180, 180, 180 });

        // banned list
        addText(m_content, "Banned", "goldFont.fnt", 0.5f, { 300.f, H - 120.f });
        y = H - 140.f;
        shown = 0;
        for (auto& name : m_banned) {
            if (shown >= 5) break;
            auto lbl = addText(m_content, name, "chatFont.fnt", 0.5f, { 225.f, y }, { 255, 140, 140 });
            lbl->setAnchorPoint({ 0.f, 0.5f });
            lbl->limitLabelWidth(90.f, 0.5f, 0.2f);
            auto unban = textButton("Unban", "GJ_button_01.png", 0.4f, [this, name] {
                this->action("unban", name, [name](std::string) {
                    Notification::create(fmt::format("Unbanned {}", name), NotificationIcon::Success)->show();
                });
            });
            unban->setPosition({ 350.f, y });
            menu->addChild(unban);
            y -= 22.f;
            shown++;
        }
        if (m_banned.size() > 5) addText(m_content, fmt::format("+{} more", m_banned.size() - 5), "chatFont.fnt", 0.4f, { 300.f, y }, { 180, 180, 180 });
        if (m_banned.empty()) addText(m_content, m_infoLoaded ? "Nobody" : "Loading...", "chatFont.fnt", 0.45f, { 300.f, y }, { 180, 180, 180 });

        addText(m_content, m_joinCode.empty() ? "No join code: any team member can open it" : "Join code is on: people need it to open this level",
            "chatFont.fnt", 0.42f, { W / 2 - 40.f, 22.f }, { 200, 200, 200 });
    }

public:
    static TeamAdminPopup* create(std::string code) {
        auto ret = new TeamAdminPopup();
        if (ret->init(code)) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Goals: what everyone wants to do on a team level
// ---------------------------------------------------------------------------

class GoalsPopup : public Popup {
protected:
    static constexpr float W = 400.f;
    static constexpr float H = 280.f;
    static constexpr float LIST_W = 360.f;
    static constexpr float LIST_H = 160.f;
    static constexpr float ROW_H = 34.f;

    std::string m_code;
    std::string m_name;
    matjson::Value m_goals;
    ScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_info = nullptr;
    TextInput* m_input = nullptr;
    async::TaskHolder<web::WebResponse> m_loadTask;
    async::TaskHolder<web::WebResponse> m_actionTask;

    bool init(std::string code, std::string name) {
        if (!Popup::init(W, H)) return false;
        m_code = code;
        m_name = name;
        this->setTitle(fmt::format("Goals: {}", name).c_str());

        auto bg = CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({ LIST_W, LIST_H });
        bg->setOpacity(80);
        bg->setPosition({ W / 2, H - 45.f - LIST_H / 2 });
        m_mainLayer->addChild(bg);

        m_scroll = ScrollLayer::create({ LIST_W, LIST_H });
        m_scroll->ignoreAnchorPointForPosition(false);
        m_scroll->setAnchorPoint({ 0.f, 0.f });
        m_scroll->setPosition({ (W - LIST_W) / 2, H - 45.f - LIST_H });
        m_mainLayer->addChild(m_scroll);
        m_info = addText(m_mainLayer, "Loading goals...", "chatFont.fnt", 0.55f, { W / 2, H - 45.f - LIST_H / 2 });

        m_input = TextInput::create(270.f, "What do you want to do? (e.g. decorate part 2)", "chatFont.fnt");
        m_input->setMaxCharCount(120);
        m_input->setFilter(" !#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`abcdefghijklmnopqrstuvwxyz{|}~\"");
        m_input->setPosition({ W / 2 - 40.f, 38.f });
        m_mainLayer->addChild(m_input);
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(menu);
        auto add = textButton("Add", "GJ_button_01.png", 0.6f, [this] { this->addGoal(); });
        add->setPosition({ W / 2 + 145.f, 38.f });
        menu->addChild(add);

        this->load();
        this->schedule(schedule_selector(GoalsPopup::autoRefresh), 5.f);
        return true;
    }

    void autoRefresh(float) { this->load(); }

    void load() {
        auto url = fmt::format("{}/rest/v1/team_goals?room=eq.{}&select=id,author,text,done,done_by&order=id.asc&limit=100", SUPA_URL, m_code);
        auto self = WeakRef<GoalsPopup>(this);
        m_loadTask.spawn(supa().get(url), [self](web::WebResponse res) {
            auto popup = self.lock();
            if (!popup) return;
            if (!res.ok()) {
                popup->m_info->setVisible(true);
                popup->m_info->setString(res.code() == 404 ? "Goals aren't set up yet (run sql/admin.sql)" : "Couldn't load goals");
                return;
            }
            popup->m_goals = res.json().unwrapOr(matjson::Value());
            popup->rebuild();
        });
    }

    void addGoal() {
        std::string text = utils::string::trim(std::string(m_input->getString()));
        if (text.empty()) return;
        auto req = supa();
        req.header("Prefer", "return=minimal");
        req.bodyJSON(matjson::makeObject({ { "room", m_code }, { "author", playerName() }, { "text", text.substr(0, 120) } }));
        auto self = WeakRef<GoalsPopup>(this);
        m_actionTask.spawn(req.post(fmt::format("{}/rest/v1/team_goals", SUPA_URL)), [self](web::WebResponse res) {
            if (!res.ok()) {
                Notification::create(fmt::format("Couldn't add the goal (HTTP {})", res.code()), NotificationIcon::Error)->show();
                return;
            }
            if (auto popup = self.lock()) {
                popup->m_input->setString("");
                popup->load();
            }
        });
    }

    void setDone(int64_t id, bool done) {
        auto req = supa();
        req.header("Prefer", "return=minimal");
        req.bodyJSON(matjson::makeObject({ { "done", done }, { "done_by", done ? playerName() : std::string() } }));
        auto self = WeakRef<GoalsPopup>(this);
        m_actionTask.spawn(req.patch(fmt::format("{}/rest/v1/team_goals?id=eq.{}", SUPA_URL, id)), [self](web::WebResponse res) {
            if (!res.ok()) Notification::create("Couldn't update the goal", NotificationIcon::Error)->show();
            if (auto popup = self.lock()) popup->load();
        });
    }

    void removeGoal(int64_t id) {
        auto self = WeakRef<GoalsPopup>(this);
        callRpc(m_actionTask, "team_goal_remove", matjson::makeObject({
            { "p_id", id }, { "p_name", playerName() }, { "p_key", ownerKeyFor(m_code) },
        }), [self](bool ok, std::string result) {
            if (!ok || result != "ok") Notification::create("Only whoever wrote it (or the admin) can remove it", NotificationIcon::Warning)->show();
            if (auto popup = self.lock()) popup->load();
        });
    }

    void rebuild() {
        auto content = m_scroll->m_contentLayer;
        content->removeAllChildren();
        if (!m_goals.isArray()) return;
        if (m_goals.size() == 0) {
            m_info->setVisible(true);
            m_info->setString("No goals yet. Write what you want to do!");
            return;
        }
        m_info->setVisible(false);
        size_t doneCount = 0;
        for (auto& g : m_goals) if (g["done"].asBool().unwrapOr(false)) doneCount++;
        this->setTitle(fmt::format("Goals: {} ({}/{} done)", m_name, doneCount, m_goals.size()).c_str());

        float total = std::max(LIST_H, ROW_H * m_goals.size());
        content->setContentSize({ LIST_W, total });
        bool admin = !ownerKeyFor(m_code).empty();
        auto me = lower(playerName());
        size_t i = 0;
        for (auto& g : m_goals) {
            int64_t id = g["id"].asInt().unwrapOr(0);
            auto text = g["text"].asString().unwrapOr("");
            auto author = g["author"].asString().unwrapOr("?");
            bool done = g["done"].asBool().unwrapOr(false);
            auto doneBy = g["done_by"].asString().unwrapOr("");
            float y = total - ROW_H * i - ROW_H / 2;

            auto row = CCNode::create();
            row->setContentSize({ LIST_W, ROW_H });
            row->setAnchorPoint({ 0.5f, 0.5f });
            row->setPosition({ LIST_W / 2, y });
            content->addChild(row);
            if (i % 2 == 0) row->addChild(CCLayerColor::create({ 0, 0, 0, 40 }, LIST_W, ROW_H));

            auto menu = CCMenu::create();
            menu->setPosition({ 0.f, 0.f });
            row->addChild(menu);
            auto self = WeakRef<GoalsPopup>(this);
            auto check = CCMenuItemExt::createSpriteExtraWithFrameName(done ? "GJ_checkOn_001.png" : "GJ_checkOff_001.png", 0.55f,
                [self, id, done](CCMenuItemSpriteExtra*) { if (auto p = self.lock()) p->setDone(id, !done); });
            check->setPosition({ 18.f, ROW_H / 2 });
            menu->addChild(check);

            auto lbl = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
            lbl->setAnchorPoint({ 0.f, 0.5f });
            lbl->setPosition({ 36.f, ROW_H - 12.f });
            lbl->limitLabelWidth(270.f, 0.55f, 0.2f);
            if (done) lbl->setColor({ 140, 255, 140 });
            row->addChild(lbl);

            auto who = done ? fmt::format("by {}  |  done by {}", author, doneBy.empty() ? "?" : doneBy) : fmt::format("by {}", author);
            auto sub = CCLabelBMFont::create(who.c_str(), "chatFont.fnt");
            sub->setAnchorPoint({ 0.f, 0.5f });
            sub->setPosition({ 36.f, 9.f });
            sub->setScale(0.4f);
            sub->setColor({ 190, 190, 190 });
            row->addChild(sub);

            if (admin || lower(author) == me) {
                auto del = CCMenuItemExt::createSpriteExtraWithFrameName("GJ_deleteIcon_001.png", 0.5f,
                    [self, id](CCMenuItemSpriteExtra*) { if (auto p = self.lock()) p->removeGoal(id); });
                del->setPosition({ LIST_W - 18.f, ROW_H / 2 });
                menu->addChild(del);
            }
            i++;
        }
    }

public:
    static GoalsPopup* create(std::string code, std::string name) {
        auto ret = new GoalsPopup();
        if (ret->init(code, name)) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Update log: what changed in each version (from changelog.md)
// ---------------------------------------------------------------------------

struct ChangelogSection {
    std::string version;
    std::string text;
};

// changelog.md is "# v1.1.2" headers, each followed by "- change" lines
static std::vector<ChangelogSection> parseChangelog(std::string const& md) {
    std::vector<ChangelogSection> out;
    for (auto& line : utils::string::split(md, "\n")) {
        auto trimmed = utils::string::trim(line);
        if (trimmed.starts_with("# ")) {
            out.push_back({ utils::string::trim(trimmed.substr(2)), "" });
        } else if (!out.empty() && !trimmed.empty()) {
            out.back().text += trimmed + "\n";
        }
    }
    return out;
}

static std::string changelogMarkdown(std::vector<ChangelogSection> const& sections) {
    std::string md;
    for (auto& sec : sections) md += fmt::format("## {}\n{}\n", sec.version, sec.text);
    return md.empty() ? std::string("No changes listed.") : md;
}

static void showUpdateLog() {
    auto md = Mod::get()->getMetadata().getChangelog().value_or("");
    MDPopup::create("Update Log", changelogMarkdown(parseChangelog(md)), "OK")->show();
}

// First time GD starts after an update: show what's new
static void showWhatsNewOnce() {
    auto current = Mod::get()->getVersion().toVString();
    auto seen = Mod::get()->getSavedValue<std::string>("seen-version", "");
    Mod::get()->setSavedValue<std::string>("seen-version", current);
    if (seen.empty() || seen == current) return; // fresh install, or nothing new
    auto sections = parseChangelog(Mod::get()->getMetadata().getChangelog().value_or(""));
    std::vector<ChangelogSection> fresh;
    auto seenVer = VersionInfo::parse(seen);
    for (auto& sec : sections) {
        auto v = VersionInfo::parse(sec.version);
        if (!v || !seenVer || v.unwrap() > seenVer.unwrap()) fresh.push_back(sec);
    }
    if (fresh.empty()) return;
    MDPopup::create(fmt::format("What's new in {}", current).c_str(), changelogMarkdown(fresh), "Nice")->show();
}

// ---------------------------------------------------------------------------
// Team Gaming: the list of the team's levels
// ---------------------------------------------------------------------------

class TeamHubPopup : public Popup {
protected:
    static constexpr float W = 420.f;
    static constexpr float H = 270.f;
    static constexpr float LIST_W = 380.f;
    static constexpr float LIST_H = 170.f;
    static constexpr float ROW_H = 42.f;

    CCLabelBMFont* m_info = nullptr;
    ScrollLayer* m_scroll = nullptr;
    async::TaskHolder<web::WebResponse> m_listTask;
    async::TaskHolder<web::WebResponse> m_onlineTask;
    matjson::Value m_projects;
    std::unordered_map<std::string, int> m_online;
    std::unordered_map<std::string, std::pair<int, int>> m_goalCounts; // done, total
    async::TaskHolder<web::WebResponse> m_goalsTask;
    async::TaskHolder<web::WebResponse> m_deleteTask;

    void deleteProject(std::string code) {
        auto self = WeakRef<TeamHubPopup>(this);
        callRpc(m_deleteTask, "team_admin", matjson::makeObject({
            { "p_code", code }, { "p_key", ownerKeyFor(code) }, { "p_action", "delete" }, { "p_value", "" },
        }), [self, code](bool ok, std::string result) {
            if (!ok || (result != "ok" && result != "missing")) {
                Notification::create(fmt::format("Couldn't delete: {}", ok ? result : "couldn't reach the server"), NotificationIcon::Error, 3.f)->show();
                return;
            }
            Mod::get()->setSavedValue<std::string>("owner-" + code, "");
            Notification::create("Team level deleted", NotificationIcon::Success)->show();
            if (auto popup = self.lock()) popup->load();
        });
    }

    bool init() {
        if (!Popup::init(W, H)) return false;
        this->setTitle("Team Gaming");

        auto bg = CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({ LIST_W, LIST_H });
        bg->setOpacity(80);
        bg->setPosition({ W / 2, H / 2 + 5.f });
        m_mainLayer->addChild(bg);

        m_scroll = ScrollLayer::create({ LIST_W, LIST_H });
        m_scroll->ignoreAnchorPointForPosition(false);
        m_scroll->setAnchorPoint({ 0.f, 0.f });
        m_scroll->setPosition({ (W - LIST_W) / 2, H / 2 + 5.f - LIST_H / 2 });
        m_mainLayer->addChild(m_scroll);

        m_info = addText(m_mainLayer, "Loading team levels...", "chatFont.fnt", 0.6f, { W / 2, H / 2 + 5.f });

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(menu);
        auto add = textButton("New team level", "GJ_button_01.png", 0.65f, [] {
            if (!isOnTeam()) {
                Notification::create("Only Gaming team members can make team levels", NotificationIcon::Warning, 3.f)->show();
                return;
            }
            if (auto p = NewTeamLevelPopup::create()) p->show();
        });
        add->setPosition({ W / 2 - 75.f, 26.f });
        menu->addChild(add);
        auto refresh = textButton("Refresh", "GJ_button_04.png", 0.65f, [this] { this->load(); });
        refresh->setPosition({ W / 2 + 70.f, 26.f });
        menu->addChild(refresh);
        auto log = textButton("Update log", "GJ_button_05.png", 0.55f, [] { showUpdateLog(); });
        log->setPosition({ W / 2 + 155.f, 26.f });
        menu->addChild(log);

        loadTeamMembers();
        this->load();
        return true;
    }

    void load() {
        m_info->setVisible(true);
        m_info->setString("Loading team levels...");
        auto url = fmt::format("{}/rest/v1/team_projects?select=code,level_name,host,saved_by,objects,updated_at,locked&order=updated_at.desc&limit=100", SUPA_URL);
        auto self = WeakRef<TeamHubPopup>(this);
        m_listTask.spawn(supa().get(url), [self](web::WebResponse res) {
            auto popup = self.lock();
            if (!popup) return;
            if (!res.ok()) {
                popup->m_info->setString(res.code() == 404
                    ? "The database isn't set up yet (run sql/setup.sql)"
                    : fmt::format("Couldn't load team levels (HTTP {})", res.code()).c_str());
                return;
            }
            popup->m_projects = res.json().unwrapOr(matjson::Value());
            popup->rebuild();
        });
        auto goalsUrl = fmt::format("{}/rest/v1/team_goals?select=room,done&limit=5000", SUPA_URL);
        m_goalsTask.spawn(supa().get(goalsUrl), [self](web::WebResponse res) {
            auto popup = self.lock();
            if (!popup || !res.ok()) return;
            popup->m_goalCounts.clear();
            auto json = res.json().unwrapOr(matjson::Value());
            if (json.isArray()) for (auto& g : json) {
                auto& c = popup->m_goalCounts[g["room"].asString().unwrapOr("")];
                c.second++;
                if (g["done"].asBool().unwrapOr(false)) c.first++;
            }
            popup->rebuild();
        });
        auto onlineUrl = fmt::format("{}/rest/v1/team_members?last_seen=gt.{}&select=room", SUPA_URL, isoNow(-20));
        m_onlineTask.spawn(supa().get(onlineUrl), [self](web::WebResponse res) {
            auto popup = self.lock();
            if (!popup || !res.ok()) return;
            popup->m_online.clear();
            auto json = res.json().unwrapOr(matjson::Value());
            if (json.isArray()) for (auto& m : json) popup->m_online[m["room"].asString().unwrapOr("")]++;
            popup->rebuild();
        });
    }

    void rebuild() {
        auto content = m_scroll->m_contentLayer;
        content->removeAllChildren();
        if (!m_projects.isArray()) return;
        if (m_projects.size() == 0) {
            m_info->setVisible(true);
            m_info->setString("No team levels yet. Make the first one!");
            return;
        }
        m_info->setVisible(false);

        float total = std::max(LIST_H, ROW_H * m_projects.size());
        content->setContentSize({ LIST_W, total });
        size_t i = 0;
        for (auto& p : m_projects) {
            auto code = p["code"].asString().unwrapOr("");
            auto name = p["level_name"].asString().unwrapOr("Team level");
            auto host = p["host"].asString().unwrapOr("?");
            auto savedBy = p["saved_by"].asString().unwrapOr("");
            auto objects = p["objects"].asInt().unwrapOr(0);
            auto updated = timeAgo(p["updated_at"].asString().unwrapOr(""));
            int online = m_online.contains(code) ? m_online[code] : 0;

            float y = total - ROW_H * i - ROW_H / 2;
            auto row = CCNode::create();
            row->setContentSize({ LIST_W, ROW_H });
            row->setAnchorPoint({ 0.5f, 0.5f });
            row->setPosition({ LIST_W / 2, y });
            content->addChild(row);

            if (i % 2 == 0) {
                auto stripe = CCLayerColor::create({ 0, 0, 0, 40 }, LIST_W, ROW_H);
                row->addChild(stripe);
            }
            auto title = CCLabelBMFont::create(name.c_str(), "goldFont.fnt");
            title->setAnchorPoint({ 0.f, 0.5f });
            title->setPosition({ 10.f, ROW_H - 14.f });
            title->limitLabelWidth(220.f, 0.6f, 0.2f);
            row->addChild(title);
            float tagX = 10.f + title->getScaledContentSize().width + 6.f;
            if (p["locked"].asBool().unwrapOr(false)) {
                if (auto lock = CCSprite::createWithSpriteFrameName("GJ_lock_001.png")) {
                    lock->setScale(0.35f);
                    lock->setPosition({ tagX + 5.f, ROW_H - 14.f });
                    row->addChild(lock);
                    tagX += 14.f;
                }
            }
            if (!ownerKeyFor(code).empty()) {
                auto tag = CCLabelBMFont::create("ADMIN", "bigFont.fnt");
                tag->setScale(0.3f);
                tag->setColor({ 255, 200, 60 });
                tag->setAnchorPoint({ 0.f, 0.5f });
                tag->setPosition({ tagX, ROW_H - 14.f });
                row->addChild(tag);
            }

            std::string sub = fmt::format("by {}  |  {} objects  |  saved {}{}", host, objects, updated,
                savedBy.empty() ? "" : fmt::format(" by {}", savedBy));
            if (auto it = m_goalCounts.find(code); it != m_goalCounts.end()) {
                sub += fmt::format("  |  goals {}/{}", it->second.first, it->second.second);
            }
            auto info = CCLabelBMFont::create(sub.c_str(), "chatFont.fnt");
            info->setAnchorPoint({ 0.f, 0.5f });
            info->setPosition({ 10.f, 11.f });
            info->limitLabelWidth(215.f, 0.45f, 0.2f);
            info->setColor({ 210, 210, 210 });
            row->addChild(info);

            if (online > 0) {
                auto on = CCLabelBMFont::create(fmt::format("{} online", online).c_str(), "chatFont.fnt");
                on->setScale(0.45f);
                on->setColor({ 120, 255, 120 });
                on->setPosition({ LIST_W - 100.f, ROW_H - 7.f });
                row->addChild(on);
            }

            auto menu = CCMenu::create();
            menu->setPosition({ 0.f, 0.f });
            row->addChild(menu);
            auto self = WeakRef<TeamHubPopup>(this);
            auto open = textButton("Open", "GJ_button_02.png", 0.55f, [self, code, name] {
                if (auto popup = self.lock()) popup->onClose(nullptr);
                tryOpenProject(code, name);
            });
            open->setPosition({ LIST_W - 40.f, ROW_H / 2 });
            menu->addChild(open);
            auto goals = textButton("Goals", "GJ_button_04.png", 0.45f, [code, name] {
                if (auto p = GoalsPopup::create(code, name)) p->show();
            });
            goals->setPosition({ LIST_W - 100.f, ROW_H / 2 });
            menu->addChild(goals);
            if (!ownerKeyFor(code).empty()) {
                // the admin can delete it right from the list
                auto del = CCMenuItemExt::createSpriteExtraWithFrameName("GJ_deleteIcon_001.png", 0.6f, [self, code, name](CCMenuItemSpriteExtra*) {
                    createQuickPopup("Delete team level",
                        fmt::format("Delete <cy>{}</c> <cr>for everyone</c>? The saved progress and goals are gone forever. (Your copy in My Levels stays.)", name),
                        "Cancel", "Delete", [self, code](auto, bool yes) {
                            auto popup = self.lock();
                            if (!yes || !popup) return;
                            popup->deleteProject(code);
                        });
                });
                del->setPosition({ LIST_W - 142.f, ROW_H / 2 });
                menu->addChild(del);
            }
            i++;
        }
        m_scroll->moveToTop();
    }

public:
    static TeamHubPopup* create() {
        auto ret = new TeamHubPopup();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// In the editor: pause menu -> Team (who's here, save, leave)
// ---------------------------------------------------------------------------

class TeamEditorPopup : public Popup {
protected:
    static constexpr float W = 330.f;
    static constexpr float H = 220.f;
    WeakRef<LevelEditorLayer> m_editor;
    CCNode* m_content = nullptr;

    bool init(LevelEditorLayer* editor) {
        if (!Popup::init(W, H)) return false;
        m_editor = editor;
        this->setTitle("Team Gaming");
        this->rebuild();
        this->schedule(schedule_selector(TeamEditorPopup::refresh), 1.f);
        return true;
    }

    void refresh(float) { this->rebuild(); }

    void rebuild() {
        if (m_content) m_content->removeFromParent();
        m_content = CCNode::create();
        m_mainLayer->addChild(m_content);
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_content->addChild(menu);
        auto& s = CollabSession::get();
        auto editor = m_editor.lock();

        if (!editor || !s.isActiveFor(editor)) {
            addText(m_content, "This isn't a team level.", "chatFont.fnt", 0.65f, { W / 2, H - 70.f });
            addText(m_content, "Open one from the Team Gaming button", "chatFont.fnt", 0.55f, { W / 2, H - 100.f }, { 200, 200, 200 });
            addText(m_content, "on the main menu.", "chatFont.fnt", 0.55f, { W / 2, H - 116.f }, { 200, 200, 200 });
            return;
        }

        addText(m_content, s.projectName, "goldFont.fnt", 0.8f, { W / 2, H - 55.f })->limitLabelWidth(W - 40.f, 0.8f, 0.3f);
        std::string who;
        for (size_t i = 0; i < s.members.size(); i++) who += (i ? ", " : "") + s.members[i];
        if (who.empty()) who = playerName();
        addText(m_content, fmt::format("Here now ({}): {}", std::max<size_t>(1, s.members.size()), who), "chatFont.fnt", 0.55f, { W / 2, H - 85.f })
            ->limitLabelWidth(W - 40.f, 0.55f, 0.2f);
        addText(m_content, s.saveText(), "chatFont.fnt", 0.55f, { W / 2, H - 105.f }, { 140, 255, 140 });
        addText(m_content, s.status, "chatFont.fnt", 0.5f, { W / 2, H - 123.f }, { 180, 220, 255 });

        auto save = textButton("Save now", "GJ_button_01.png", 0.55f, [this] {
            if (auto ed = m_editor.lock()) {
                CollabSession::get().saveNow(ed, false);
                Notification::create("Saving...", NotificationIcon::Loading, 1.f)->show();
            }
        });
        save->setPosition({ 50.f, 55.f });
        menu->addChild(save);
        auto goals = textButton("Goals", "GJ_button_04.png", 0.55f, [] {
            auto& s = CollabSession::get();
            if (auto p = GoalsPopup::create(s.room, s.projectName)) p->show();
        });
        goals->setPosition({ 128.f, 55.f });
        menu->addChild(goals);
        if (!ownerKeyFor(s.room).empty()) {
            auto admin = textButton("Admin", "GJ_button_03.png", 0.55f, [] {
                if (auto p = TeamAdminPopup::create(CollabSession::get().room)) p->show();
            });
            admin->setPosition({ 200.f, 55.f });
            menu->addChild(admin);
        }
        auto leave = textButton("Stop", "GJ_button_06.png", 0.55f, [this] {
            if (auto ed = m_editor.lock()) CollabSession::get().saveNow(ed, true);
            CollabSession::get().leave();
            this->rebuild();
        });
        leave->setPosition({ 275.f, 55.f });
        menu->addChild(leave);
        addText(m_content, "Progress saves online by itself. Exiting the editor saves too.", "chatFont.fnt", 0.42f, { W / 2, 22.f }, { 200, 200, 200 });
    }

public:
    static TeamEditorPopup* create(LevelEditorLayer* editor) {
        auto ret = new TeamEditorPopup();
        if (ret->init(editor)) { ret->autorelease(); return ret; }
        delete ret;
        return nullptr;
    }
};

class $modify(TeamPauseLayer, EditorPauseLayer) {
    void customSetup() {
        EditorPauseLayer::customSetup();
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto editor = WeakRef<LevelEditorLayer>(m_editorLayer);
        auto btn = textButton("Team", "GJ_button_02.png", 0.7f, [editor] {
            if (auto ed = editor.lock()) {
                if (auto popup = TeamEditorPopup::create(ed)) popup->show();
            }
        });
        auto menu = CCMenu::create();
        menu->setID("team-menu"_spr);
        menu->setPosition({ 0.f, 0.f });
        // under where Gaming Teams puts its button, so both mods fit
        btn->setPosition({ winSize.width - 112.f, winSize.height - 88.f });
        menu->addChild(btn);
        this->addChild(menu, 100);
    }
};

// ---------------------------------------------------------------------------
// Self-updater (same as the other mods)
// ---------------------------------------------------------------------------

#if GEODE_COMP_GD_VERSION >= 22091
static constexpr auto RELEASE_BASE = "https://github.com/pester33/Teams-yay/releases/download/latest-build-2.2091/";
#else
static constexpr auto RELEASE_BASE = "https://github.com/pester33/Teams-yay/releases/download/latest-build/";
#endif

static async::TaskHolder<web::WebResponse> s_updateTask;
static bool s_updateBusy = false;

static void whenScreenReady(std::function<void()> fn, int tries = 0) {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    bool ready = scene && !typeinfo_cast<CCTransitionScene*>(scene);
    if (ready || tries > 600) {
        fn();
        return;
    }
    Loader::get()->queueInMainThread([fn = std::move(fn), tries]() mutable {
        whenScreenReady(std::move(fn), tries + 1);
    });
}

static void installUpdate(std::string version) {
    if (s_updateBusy) return;
    s_updateBusy = true;
    auto url = fmt::format("{}{}.geode", RELEASE_BASE, Mod::get()->getID());
    auto loading = Notification::create("Downloading Gaming Teams update...", NotificationIcon::Loading, 0.f);
    loading->show();
    web::WebRequest req;
    setTimeout(req, 90);
    s_updateTask.spawn(req.get(url), [loading = Ref(loading), version](web::WebResponse res) {
        s_updateBusy = false;
        loading->hide();
        auto const& data = res.data();
        if (!res.ok() || data.size() < 4 || data[0] != 'P' || data[1] != 'K') {
            Notification::create(fmt::format("Update failed to download (HTTP {})", res.code()), NotificationIcon::Error, 3.f)->show();
            return;
        }
        if (!file::writeBinarySafe(Mod::get()->getPackagePath(), data)) {
            Notification::create("Update failed to install", NotificationIcon::Error, 3.f)->show();
            return;
        }
        whenScreenReady([version] {
            createQuickPopup("Update installed", fmt::format("Gaming Teams <cg>{}</c> is installed.\nRestart GD to use it.", version),
                "Later", "Restart", [](auto, bool restart) { if (restart) game::restart(true); });
        });
    });
}

static constexpr auto CHANGELOG_URL = "https://raw.githubusercontent.com/pester33/Teams-yay/main/changelog.md";

static void checkForUpdate(bool isRetry = false) {
    if (s_updateBusy) return;
    s_updateBusy = true;
    web::WebRequest req;
    setTimeout(req, 20);
    s_updateTask.spawn(req.get(fmt::format("{}version.txt", RELEASE_BASE)), [isRetry](web::WebResponse res) {
        s_updateBusy = false;
        auto body = utils::string::trim(res.string().unwrapOr(""));
        log::info("[updater] version.txt: HTTP {}, body '{}'", res.code(), body.substr(0, 40));
        if (!res.ok()) {
            if (!isRetry) Loader::get()->queueInMainThread([] { checkForUpdate(true); });
            return;
        }
        auto latest = VersionInfo::parse(body);
        if (!latest) return;
        auto current = Mod::get()->getVersion();
        if (!(latest.unwrap() > current)) return;
        auto version = latest.unwrap().toVString();
        // grab the new version's changelog so the popup can show what's new
        web::WebRequest logReq;
        setTimeout(logReq, 15);
        s_updateTask.spawn(logReq.get(CHANGELOG_URL), [version, current](web::WebResponse logRes) {
            std::vector<ChangelogSection> fresh;
            if (logRes.ok()) {
                for (auto& sec : parseChangelog(logRes.string().unwrapOr(""))) {
                    auto v = VersionInfo::parse(sec.version);
                    if (v && v.unwrap() > current) fresh.push_back(sec);
                }
            }
            whenScreenReady([version, current, fresh] {
                std::string md = fmt::format("A new version is out: **{}** (you have {}).\n\n", version, current.toVString());
                md += fresh.empty() ? std::string("Update now?") : changelogMarkdown(fresh);
                MDPopup::create("Update available", md, "Later", "Update", [version](bool update) {
                    if (update) installUpdate(version);
                })->show();
            });
        });
    });
}

class $modify(CollabMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        auto spr = CircleButtonSprite::createWithSpriteFrameName("accountBtn_friends_001.png", 1.f, CircleBaseColor::Green, CircleBaseSize::MediumAlt);
        CCNode* look = spr;
        if (!spr) look = ButtonSprite::create("Team", "goldFont.fnt", "GJ_button_01.png", 0.8f);
        auto btn = CCMenuItemExt::createSpriteExtra(look, [](CCMenuItemSpriteExtra*) {
            if (auto popup = TeamHubPopup::create()) popup->show();
        });
        btn->setID("team-gaming-button"_spr);
        if (auto bottom = this->getChildByID("bottom-menu")) {
            bottom->addChild(btn);
            bottom->updateLayout();
        } else {
            auto menu = CCMenu::create();
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            menu->setPosition({ winSize.width / 2, 45.f });
            menu->addChild(btn);
            this->addChild(menu);
        }
        loadTeamMembers();
        static bool firstTime = true;
        if (firstTime) {
            firstTime = false;
            if (Mod::get()->getSettingValue<bool>("check-updates")) checkForUpdate();
            Loader::get()->queueInMainThread([] { showWhatsNewOnce(); });
        }
        return true;
    }
};
