#pragma once

#include <string>
#include <string_view>

class CommandOutput;
class Player;

namespace creatorx {

// 操作结果的输出位置：命令输出，或者直接发给玩家（表单里执行时）。
// 表单可传入 errors，把错误收集起来显示在重新打开的表单上。
class Feedback {
public:
    Feedback(CommandOutput& output) : mOutput(&output) {}
    explicit Feedback(Player& player, std::string* errors = nullptr) : mPlayer(&player), mErrors(errors) {}

    void success(std::string_view text) const;
    void error(std::string_view text) const;

private:
    CommandOutput* mOutput = nullptr;
    Player*        mPlayer = nullptr;
    std::string*   mErrors = nullptr;
};

} // namespace creatorx
