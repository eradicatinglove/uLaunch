#include <ul/system/app/app_ControlCache.hpp>
#include <ul/ul_Result.hpp>
#include <ul/util/util_Scope.hpp>
#include <ul/util/util_Size.hpp>
#include <queue>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <ul/fs/fs_Stdio.hpp>
#include <vector>   // for icon override buffer
#include <string>   // for config.ini parsing
#include <sstream>  // for config.ini parsing

using namespace ul::util::size;

namespace ul::system::app {

    namespace {

        SetLanguage g_SystemLanguage;

        std::queue<u64> *g_PendingApplicationCacheQueue;
        ul::Mutex g_PendingApplicationCacheQueueLock;

        std::queue<u64> *g_PendingApplicationRemovalCacheQueue;
        ul::Mutex g_PendingApplicationRemovalCacheQueueLock;

        std::unordered_map<u64, ApplicationNacpMisc> *g_ApplicationCache;
        ul::Mutex g_ApplicationCacheLock;

        Thread g_ApplicationControlCacheThread;
        constexpr size_t ApplicationControlCacheThreadStackSize = 64_KB;

        constexpr size_t PerApplicationCacheUsage = sizeof(ApplicationNacpMisc) + sizeof(u64);
        constexpr double PerApplicationCacheUsageMb = PerApplicationCacheUsage / 1024.0f / 1024.0f;

        constexpr size_t LoopQueryMaxRetryCount = 50;

        // Max size we'll accept from sys-icon override files
        constexpr size_t MaxOverrideIconSize   = 512_KB;
        constexpr size_t MaxConfigIniSize      = 4096; // 4 KB is plenty for simple ini

        void LogCacheMemoryUsage() {
            const auto total_size = PerApplicationCacheUsage * g_ApplicationCache->size();
            UL_LOG_INFO("[ApplicationControlCache] Cache memory usage: %f MB", total_size / 1024.0f / 1024.0f);
        }

        void RemoveNextApplicationCache() {
            ScopedLock lock(g_PendingApplicationRemovalCacheQueueLock);

            if(!g_PendingApplicationRemovalCacheQueue->empty()) {
                UL_LOG_INFO("[ApplicationControlCache] Pending remove applications: %zu", g_PendingApplicationRemovalCacheQueue->size());
                const auto app_id = g_PendingApplicationRemovalCacheQueue->front();
                g_PendingApplicationRemovalCacheQueue->pop();

                ScopedLock lock2(g_ApplicationCacheLock);
                const auto app_it = std::find_if(g_ApplicationCache->begin(), g_ApplicationCache->end(), [app_id](const auto &entry) {
                    return entry.first == app_id;
                });
                if(app_it != g_ApplicationCache->end()) {
                    g_ApplicationCache->erase(app_it);
                    UL_LOG_INFO("[ApplicationControlCache] Removed application cache for application ID 0x%016lX", app_id);
                    LogCacheMemoryUsage();
                }
                else {
                    UL_LOG_WARN("[ApplicationControlCache] Failed to remove application cache for application ID 0x%016lX, not found in cache", app_id);
                }
            }
        }

        inline Result GetApplicationControlData(const u64 app_id, const bool force_ns, NsApplicationControlData *out_data, size_t &out_icon_size) {
            size_t got_size;
            Result rc;
            if(!force_ns && hosversionAtLeast(20,0,0)) {
                SetLanguage lang;
                rc = ncmextReadApplicationControlDataManual(g_SystemLanguage, app_id, out_data, sizeof(*out_data), &got_size, &lang);
                if(R_SUCCEEDED(rc)) {
                    UL_LOG_INFO("[ApplicationControlCache] Read manually application control data for application ID 0x%016lX with language %d", app_id, lang);
                }
            }
            else {
                rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, app_id, out_data, sizeof(*out_data), &got_size);
                if(R_SUCCEEDED(rc)) {
                    UL_LOG_INFO("[ApplicationControlCache] Got from NS application control data for application ID 0x%016lX", app_id);
                }
            }
            if(R_SUCCEEDED(rc)) {
                UL_ASSERT_TRUE(got_size <= sizeof(*out_data));
                out_icon_size = got_size - sizeof(NacpStruct);
            }
            return rc;
        }

        // -----------------------------
        //  Small helpers for config.ini
        // -----------------------------
        std::string TrimString(const std::string &in) {
            const char *ws = " \t\r\n";
            const auto start = in.find_first_not_of(ws);
            if(start == std::string::npos) {
                return "";
            }
            const auto end = in.find_last_not_of(ws);
            return in.substr(start, end - start + 1);
        }

        std::string ToLower(const std::string &in) {
            std::string out = in;
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        void CopyOverrideField(char *dst, size_t dst_size, const std::string &value) {
            if(dst == nullptr || dst_size == 0) {
                return;
            }
            std::memset(dst, 0, dst_size);
            const auto copy_len = std::min(dst_size - 1, value.size());
            if(copy_len > 0) {
                std::memcpy(dst, value.data(), copy_len);
            }
        }

        // Apply overrides from:
        //   sdmc:/atmosphere/contents/<program_id>/config.ini
        // to NacpMetadata (name, author, version), if present.
        bool ApplyConfigOverrides(const u64 app_id, smi::sf::NacpMetadata &meta) {
            const auto program_id_str = util::FormatProgramId(app_id);
            const auto override_dir   = fs::JoinPath("sdmc:/atmosphere/contents", program_id_str);
            const auto ini_path       = fs::JoinPath(override_dir, "config.ini");

            std::vector<char> buf;
            buf.resize(MaxConfigIniSize + 1);
            size_t read_size = 0;

            if(!fs::ReadAllFile(ini_path, buf.data(), MaxConfigIniSize, read_size)) {
                // No config.ini -> no overrides
                return false;
            }

            buf[read_size] = '\0';
            std::string content(buf.data(), read_size);
            std::stringstream ss(content);
            std::string line;

            std::string name_override;
            std::string author_override;
            std::string version_override;

            while(std::getline(ss, line)) {
                line = TrimString(line);
                if(line.empty()) {
                    continue;
                }
                // Skip comments and section headers
                if(line[0] == '#' || line[0] == ';' || line[0] == '[') {
                    continue;
                }

                const auto eq_pos = line.find('=');
                if(eq_pos == std::string::npos) {
                    continue;
                }

                auto key   = TrimString(line.substr(0, eq_pos));
                auto value = TrimString(line.substr(eq_pos + 1));

                key = ToLower(key);

                if(key == "name") {
                    name_override = value;
                }
                else if(key == "author") {
                    author_override = value;
                }
                else if(key == "version") {
                    version_override = value;
                }
            }

            bool applied = false;

            if(!name_override.empty()) {
                CopyOverrideField(meta.name, sizeof(meta.name), name_override);
                applied = true;
            }
            if(!author_override.empty()) {
                CopyOverrideField(meta.author, sizeof(meta.author), author_override);
                applied = true;
            }
            if(!version_override.empty()) {
                CopyOverrideField(meta.display_version, sizeof(meta.display_version), version_override);
                applied = true;
            }

            if(applied) {
                UL_LOG_INFO("[ApplicationControlCache] Applied config.ini overrides for application ID 0x%016lX", app_id);
            } else {
                UL_LOG_INFO("[ApplicationControlCache] config.ini found but no valid overrides for application ID 0x%016lX", app_id);
            }

            return applied;
        }

        // ===========================
        //  sys-icon integration here
        // ===========================
        bool CacheApplicationIcon(const u64 app_id, const u8 *icon_buf, const size_t icon_buf_size) {
            const auto program_id_str = util::FormatProgramId(app_id);
            const auto cache_path     = fs::JoinPath(ApplicationCachePath, program_id_str + ".jpg");

            /*
             * sys-icon override:
             *
             * If an override icon exists at:
             *   sdmc:/atmosphere/contents/<program_id>/icon.jpg
             *
             * we read that file and cache *it* instead of the NS icon.
             * This lets your sysmodule control uLaunch icons the same way
             * it does for qlaunch, without touching UI code.
             */
            const auto override_dir  = fs::JoinPath("sdmc:/atmosphere/contents", program_id_str);
            const auto override_path = fs::JoinPath(override_dir, "icon.jpg");

            std::vector<u8> override_buf;
            override_buf.resize(MaxOverrideIconSize);
            size_t override_size = 0;

            if(fs::ReadAllFile(override_path, override_buf.data(), override_buf.size(), override_size)) {
                UL_LOG_INFO("[ApplicationControlCache] Using sys-icon override for application ID 0x%016lX (icon size: %zu bytes)", app_id, override_size);
                return fs::WriteFile(cache_path, override_buf.data(), override_size, true);
            }

            // Fallback: original uLaunch behavior
            return fs::WriteFile(cache_path, icon_buf, icon_buf_size, true);
        }

        bool LoadApplicationIconCache(const u64 app_id, u8 *out_icon_buf, const size_t icon_buf_size, size_t &out_actual_icon_size) {
            const auto path = fs::JoinPath(ApplicationCachePath, util::FormatProgramId(app_id) + ".jpg");
            return fs::ReadAllFile(path, out_icon_buf, icon_buf_size, out_actual_icon_size);
        }

        bool CacheApplicationNacpMetadata(const u64 app_id, const smi::sf::NacpMetadata &nacp_metadata) {
            const auto path = fs::JoinPath(ApplicationCachePath, util::FormatProgramId(app_id) + ".nacp.meta");
            return fs::WriteFile(path, &nacp_metadata, sizeof(nacp_metadata), true);
        }

        bool LoadApplicationNacpMetadataCache(const u64 app_id, smi::sf::NacpMetadata &out_nacp_metadata) {
            const auto path = fs::JoinPath(ApplicationCachePath, util::FormatProgramId(app_id) + ".nacp.meta");
            size_t dummy;
            if(!fs::ReadAllFile(path, &out_nacp_metadata, sizeof(out_nacp_metadata), dummy)) {
                return false;
            }
            return dummy == sizeof(out_nacp_metadata);
        }

        void AddNextApplicationCache() {
            ScopedLock lock(g_PendingApplicationCacheQueueLock);

            if(!g_PendingApplicationCacheQueue->empty()) {
                UL_LOG_INFO("[ApplicationControlCache] Pending add applications: %zu", g_PendingApplicationCacheQueue->size());
                const auto app_id = g_PendingApplicationCacheQueue->front();

                {
                    ScopedLock lock2(g_ApplicationCacheLock);
                    auto &nacp_misc = g_ApplicationCache->operator[](app_id);

                    u32 tries = 0;
                    size_t icon_size;
                    while(true) {
                        const auto start_tick = armGetSystemTick();
                        auto control_data = new NsApplicationControlData();
                        UL_ON_SCOPE_EXIT({
                            delete control_data;
                        });

                        const auto rc = GetApplicationControlData(app_id, tries >= 5, control_data, icon_size);
                        const auto end_tick = armGetSystemTick();
                        const auto elapsed_time_ms = armTicksToNs(end_tick - start_tick) / 1'000'000;
                        if(R_FAILED(rc)) {
                            UL_LOG_WARN("[ApplicationControlCache] Failed to get control data for application ID 0x%016lX: %s (elapsed time: %ld ms)", app_id, util::FormatResultDisplay(rc).c_str(), elapsed_time_ms);
                            if(tries >= 50) {
                                UL_LOG_WARN("[ApplicationControlCache] Failed to get control data for application ID 0x%016lX after 50 tries, giving up", app_id);
                                g_ApplicationCache->erase(app_id);
                                break;
                            }
                        }
                        else {
                            UL_LOG_WARN("[ApplicationControlCache] Got control data for application ID 0x%016lX (elapsed time: %ld ms), icon size: %zu bytes", app_id, elapsed_time_ms, icon_size);

                            smi::sf::NacpMetadata meta = {};
                            NacpLanguageEntry *langentry;
                            nacpGetLanguageEntry(&control_data->nacp, &langentry);
                            if(langentry == nullptr) {
                                UL_LOG_WARN("[ApplicationControlCache] Failed to get language entry for application ID 0x%016lX, cannot cache", app_id);
                                g_ApplicationCache->erase(app_id);
                                break;
                            }
                            memcpy(meta.name, langentry->name, sizeof(meta.name));
                            memcpy(meta.author, langentry->author, sizeof(meta.author));
                            memcpy(meta.display_version, control_data->nacp.display_version, sizeof(meta.display_version));

                            // Apply config.ini overrides (if present)
                            ApplyConfigOverrides(app_id, meta);

                            // Populate nxtc cache
                            if(!CacheApplicationNacpMetadata(app_id, meta)) {
                                UL_LOG_WARN("[ApplicationControlCache] Failed to add entry to nxtc cache for application ID 0x%016lX", app_id);
                            }
                            else if(!CacheApplicationIcon(app_id, control_data->icon, icon_size)) {
                                UL_LOG_WARN("[ApplicationControlCache] Failed to cache application icon for application ID 0x%016lX", app_id);
                            }
                            else {
                                UL_LOG_INFO("[ApplicationControlCache] Added entry to nxtc cache for application ID 0x%016lX (icon size: %zu bytes)", app_id, icon_size);
                            }

                            // Populate our misc cache
                            memcpy(nacp_misc.display_version, control_data->nacp.display_version, sizeof(nacp_misc.display_version));
                            nacp_misc.video_capture = control_data->nacp.video_capture;
                            nacp_misc.save_data_owner_id = control_data->nacp.save_data_owner_id;
                            nacp_misc.user_account_save_data_size = control_data->nacp.user_account_save_data_size;
                            nacp_misc.user_account_save_data_journal_size = control_data->nacp.user_account_save_data_journal_size;
                            nacp_misc.device_save_data_size = control_data->nacp.device_save_data_size;
                            nacp_misc.device_save_data_journal_size = control_data->nacp.device_save_data_journal_size;
                            nacp_misc.temporary_storage_size = control_data->nacp.temporary_storage_size;
                            nacp_misc.cache_storage_size = control_data->nacp.cache_storage_size;
                            nacp_misc.cache_storage_journal_size = control_data->nacp.cache_storage_journal_size;
                            nacp_misc.bcat_delivery_cache_storage_size = control_data->nacp.bcat_delivery_cache_storage_size;
                            UL_LOG_INFO("[ApplicationControlCache] Added application misc cache for application ID 0x%016lX (elapsed time: %ld ms)", app_id, elapsed_time_ms);
                            LogCacheMemoryUsage();
                            break;
                        }

                        tries++;
                    }
                }

                g_PendingApplicationCacheQueue->pop();
            }
        }

        void ApplicationControlCacheMain(void*) {
            UL_LOG_INFO("[ApplicationControlCache] alive!");

            while(true) {
                RemoveNextApplicationCache();
                AddNextApplicationCache();

                svcSleepThread(10'000'000);
            }
        }

    }

    void InitializeControlCache(const std::vector<NsExtApplicationRecord> &records) {
        fs::CreateDirectory(ApplicationCachePath);

        UL_RC_ASSERT(setInitialize());
        u64 lang_code;
        UL_RC_ASSERT(setGetSystemLanguage(&lang_code));
        UL_RC_ASSERT(setMakeLanguage(lang_code, &g_SystemLanguage));
        setExit();

        UL_RC_ASSERT(ncmInitialize());

        UL_LOG_INFO("[ApplicationControlCache] Initializing control cache with %zu records (cache usage per application: %f MB)", records.size(), PerApplicationCacheUsageMb);

        g_PendingApplicationCacheQueue = new std::queue<u64>();
        g_PendingApplicationRemovalCacheQueue = new std::queue<u64>();
        g_ApplicationCache = new std::unordered_map<u64, ApplicationNacpMisc>();

        for(const auto &record : records) {
            g_PendingApplicationCacheQueue->push(record.id);
        }

        UL_RC_ASSERT(threadCreate(&g_ApplicationControlCacheThread, ApplicationControlCacheMain, nullptr, nullptr, ApplicationControlCacheThreadStackSize, 0x1F, -2));
        UL_RC_ASSERT(threadStart(&g_ApplicationControlCacheThread));
    }

    bool IsQueryLocked() {
        return g_ApplicationCacheLock.IsLocked();
    }

    void RequestCacheApplication(const u64 app_id) {
        ScopedLock lock(g_PendingApplicationCacheQueueLock);
        g_PendingApplicationCacheQueue->push(app_id);
    }

    void RequestRemoveApplicationCache(const u64 app_id) {
        ScopedLock lock(g_PendingApplicationRemovalCacheQueueLock);
        g_PendingApplicationRemovalCacheQueue->push(app_id);
    }

    // Blazingly fast compared to 20.x NS commands

    bool QueryApplicationNacp(const u64 app_id, smi::sf::NacpMetadata &out_nacp) {
        if(!LoadApplicationNacpMetadataCache(app_id, out_nacp)) {
            UL_LOG_WARN("Failed to load NACP metadata cache for application ID 0x%016lX!", app_id);
            return false;
        }
        return true;
    }

    bool QueryApplicationIcon(const u64 app_id, u8 *out_icon_buf, const size_t icon_buf_size, size_t &out_actual_icon_size) {
        if(!LoadApplicationIconCache(app_id, out_icon_buf, icon_buf_size, out_actual_icon_size)) {
            UL_LOG_WARN("Failed to load application icon cache for application ID 0x%016lX!", app_id);
            return false;
        }
        return true;
    }

    bool QueryApplicationNacpMisc(const u64 app_id, ApplicationNacpMisc &out_nacp_misc) {
        ScopedLock lock(g_ApplicationCacheLock);

        const auto app_it = std::find_if(g_ApplicationCache->begin(), g_ApplicationCache->end(), [app_id](const auto &entry) {
            return entry.first == app_id;
        });
        if(app_it != g_ApplicationCache->end()) {
            out_nacp_misc = app_it->second;
            return true;
        }

        return false;
    }

    bool LoopQueryApplicationNacpMisc(const u64 app_id, ApplicationNacpMisc &out_nacp_misc) {
        u32 i = 0;
        while(true) {
            if(IsQueryLocked()) {
                continue;
            }

            if(QueryApplicationNacpMisc(app_id, out_nacp_misc)) {
                return true;
            }
            else {
                UL_LOG_WARN("Failed to query misc NACP fields for application ID 0x%016lX!", app_id);
                i++;
                if(i >= LoopQueryMaxRetryCount) {
                    UL_LOG_WARN("Max retry count reached for application ID 0x%016lX!", app_id);
                    return false;
                }
            }

            svcSleepThread(10'000'000);
        }
    }

}

