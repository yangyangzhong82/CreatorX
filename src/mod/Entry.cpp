#include "mod/Entry.h"

#include "Config/ConfigManager.h"
#include "I18n/I18n.h"
#include "command/EditCommands.h"
#include "command/AdvancedCommands.h"
#include "command/SelectionCommands.h"
#include "exec/JobScheduler.h"
#include "input/Shortcuts.h"
#include "net/Enhancements.h"
#include "visual/PastePreview.h"
#include "storage/Nbt.h"
#include "session/PlayerSession.h"
#include "ll/api/mod/RegisterHelper.h"
#include <filesystem>

namespace creatorx {
ll::io::Logger& logger = Entry::getInstance().getSelf().getLogger();
Config&         config = ConfigManager::getInstance().get();

Entry& Entry::getInstance() {
    static Entry instance;
    return instance;
}

bool Entry::load() {
    getSelf().getLogger().debug("Loading...");
    auto langPath = getSelf().getLangDir();
    if (!I18n::getInstance().load(langPath, "zh_CN")) {
        logger.warn("语言文件未加载，将使用键名作为回退文本。");
    }

    auto configPath = getSelf().getConfigDir();
    if (!std::filesystem::exists(configPath)) {
        std::filesystem::create_directories(configPath);
    }
    configPath /= "config.jsonc";
    configPath.make_preferred();

    // 首次启动使用随模组发布的推荐配置，保留作者填写的注释。
    const auto defaultConfigPath = getSelf().getModDir() / "config.jsonc";
    if (!std::filesystem::exists(configPath) && std::filesystem::exists(defaultConfigPath)) {
        std::filesystem::copy_file(defaultConfigPath, configPath);
    }

    if (!ConfigManager::getInstance().load(configPath)) {
        getSelf().getLogger().error(tr("config.load_error"));
        return false;
    }
    I18n::getInstance().setLanguage(config.language);

#ifdef LL_PLAT_C
    // 原版初始化键盘布局时会读取已注册的快捷键。
    registerShortcuts();
#endif
    registerEnhancementProtocol();
    logger.info(tr("mod.loaded"));
    return true;
}

bool Entry::enable() {
    getSelf().getLogger().debug("Enabling...");
    enableSessions();
    exec::enableScheduler();
    enableSelectionCommands();
    enableEditCommands();
    enableAdvancedCommands();
    enableEnhancements();
    enablePreviews();
    logger.info(tr("mod.enabled"));
    return true;
}

bool Entry::disable() {
    getSelf().getLogger().debug("Disabling...");
    disableEditCommands();
    disableAdvancedCommands();
    disableEnhancements();
    disablePreviews();
    disableSelectionCommands();
    // 先结束任务（会把已完成的改动写入历史），再丢弃会话。
    exec::disableScheduler();
    disableSessions();
    storage::stopStorage();
    logger.info(tr("mod.unloaded"));
    return true;
}

bool Entry::unload() {
    getSelf().getLogger().debug("Unloading...");
    unregisterEnhancementProtocol();
    return true;
}

LL_REGISTER_MOD(creatorx::Entry, creatorx::Entry::getInstance());
} // namespace creatorx
