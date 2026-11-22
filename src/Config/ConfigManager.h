#pragma once

#include "Config.h"
#include <filesystem>
#include <memory>


namespace creatorx {

class ConfigManager {
public:
    static ConfigManager& getInstance();
    bool                  load(const std::filesystem::path& path);
    bool                  reload();
    bool                  save();
    Config&               get();
    const Config&         get() const;
    std::filesystem::path const& path() const { return mConfigPath; }

private:
    ConfigManager();
    ~ConfigManager();

    ConfigManager(const ConfigManager&)            = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&)                 = delete;
    ConfigManager& operator=(ConfigManager&&)      = delete;

    std::unique_ptr<Config> mConfig;
    std::filesystem::path   mConfigPath;
};

} // namespace creatorx
