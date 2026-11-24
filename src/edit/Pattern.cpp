#include "edit/Pattern.h"

#include "mod/Global.h"
#include "edit/Clipboard.h"
#include "session/PlayerSession.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/biome/Biome.h"

#include "mc/deps/core/string/HashedString.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/registry/BlockTypeRegistry.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_set>

namespace creatorx::edit {
namespace {
std::unordered_set<std::string> deniedBlocks;
bool blacklistReady=false;

std::string_view trim(std::string_view text) {
    auto begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    auto end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

std::string_view unquote(std::string_view text) {
    text = trim(text);
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front()) {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

// 在方括号与引号之外按分隔符切分。
template <class Pred>
std::vector<std::string_view> splitTopLevel(std::string_view text, Pred isDelimiter) {
    std::vector<std::string_view> parts;
    int                           depth = 0;
    char                          quote = 0;
    size_t                        start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
        } else if (depth == 0 && isDelimiter(c)) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

std::string normalizeName(std::string_view name) {
    std::string result(trim(name));
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (result.find(':') == std::string::npos) result.insert(0, "minecraft:");
    return result;
}

std::string_view shortName(std::string_view name) {
    return name.starts_with("minecraft:") ? name.substr(10) : name;
}

size_t editDistance(std::string_view a, std::string_view b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0]          = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t above = row[j];
            row[j]       = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] != b[j - 1])});
            diagonal     = above;
        }
    }
    return row[b.size()];
}

std::optional<std::string> suggestBlock(std::string_view name) {
    auto        target = shortName(name);
    size_t      best   = std::max<size_t>(2, target.size() / 3) + 1;
    std::string result;
    for (auto const& [key, type] : *BlockTypeRegistry::get().mBlockLookupMap) {
        auto const& candidate = key.getString();
        auto        distance  = editDistance(target, shortName(candidate));
        if (distance < best) {
            best   = distance;
            result = candidate;
        }
    }
    if (result.empty()) return std::nullopt;
    return result;
}

std::optional<Block::BlockStatesType> parseStates(std::string_view text) {
    Block::BlockStatesType states;
    if (trim(text).empty()) return states;
    for (auto entry : splitTopLevel(text, [](char c) { return c == ','; })) {
        auto equal = entry.find('=');
        if (equal == std::string_view::npos) return std::nullopt;
        auto key   = std::string(unquote(entry.substr(0, equal)));
        auto raw   = trim(entry.substr(equal + 1));
        auto value = unquote(raw);
        if (key.empty() || value.empty()) return std::nullopt;

        int  number{};
        auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (raw == "true" || raw == "false") {
            states.emplace_back(key, raw == "true");
        } else if (value.size() == raw.size() && ec == std::errc{} && end == value.data() + value.size()) {
            states.emplace_back(key, number);
        } else {
            states.emplace_back(key, std::string(value));
        }
    }
    return states;
}

struct ParsedBlock {
    Block const* block    = nullptr;
    bool         hasState = false;
};

ParsedBlock parseBlockSpec(std::string_view text, Player const& player, std::string& error) {
    text = trim(text);
    if (text.empty()) {
        error = tr("pattern.empty");
        return {};
    }
    if (text == "hand" || text == "offhand") {
        auto const& item = text == "hand" ? player.getSelectedItem() : player.getOffhandSlot();
        if (item.isNull() || !item.mBlock) {
            error = tr("pattern.no_hand_block");
            return {};
        }
        return {item.mBlock, true};
    }

    auto             bracket = text.find('[');
    std::string_view stateText;
    if (bracket != std::string_view::npos) {
        if (text.back() != ']') {
            error = tr("pattern.bad_syntax", text);
            return {};
        }
        stateText = text.substr(bracket + 1, text.size() - bracket - 2);
        text      = text.substr(0, bracket);
    }
    auto name     = normalizeName(text);
    auto fallback = Block::tryGetFromRegistry(HashedString(name));
    if (!fallback) {
        if (auto hint = suggestBlock(name)) error = tr("pattern.unknown_block_hint", name, *hint);
        else error = tr("pattern.unknown_block", name);
        return {};
    }
    if (bracket == std::string_view::npos) return {fallback.as_ptr(), false};

    auto states = parseStates(stateText);
    auto block  = states ? Block::tryGetFromRegistry(HashedString(name), *states) : std::nullopt;
    if (!block) {
        error = tr("pattern.bad_states", name, stateText);
        return {};
    }
    return {block.as_ptr(), true};
}

} // namespace

Block const& airBlock() {
    static Block const* air = Block::tryGetFromRegistry(HashedString("minecraft:air")).as_ptr();
    return *air;
}

bool isDisallowed(Block const& block) {
    if(!blacklistReady) refreshBlockBlacklist();
    return deniedBlocks.contains(block.getTypeName());
}

void refreshBlockBlacklist() {
    deniedBlocks.clear();
    for(auto const& name:config.limits.disallowedBlocks) deniedBlocks.insert(normalizeName(name));
    blacklistReady=true;
}

std::vector<std::string> registeredBlockIds() {
    auto const& blocks = *BlockTypeRegistry::get().mBlockLookupMap;
    std::vector<std::string> ids;
    ids.reserve(blocks.size());
    for (auto const& [id, type] : blocks) ids.push_back(id.getString());
    std::ranges::sort(ids);
    return ids;
}

Block const* parseBlock(std::string_view text, Player const& player, std::string& error) {
    return parseBlockSpec(text, player, error).block;
}

std::vector<std::string_view> splitTokens(std::string_view text) {
    std::vector<std::string_view> tokens;
    for (auto token : splitTopLevel(text, [](char c) { return c == ' ' || c == '\t'; })) {
        if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
}

std::optional<Pattern> Pattern::parse(std::string_view text, Player const& player, std::string& error) {
    text=trim(text);
    if(text == "#clipboard") {
        auto session=findSession(player.getUuid());
        if(!session || !session->clipboard) { error=tr("clipboard.empty"); return std::nullopt; }
        Pattern result;
        result.mClipboard=session->clipboard;
        if(!result.isAllowed()) { error=tr("pattern.clipboard_denied"); return std::nullopt; }
        return result;
    }
    if(text.starts_with("#[")) { error=tr("pattern.bad_syntax",text); return std::nullopt; }
    if(text.starts_with("#gradient[") && text.ends_with(']') && splitTopLevel(text,[](char c){return c==',';}).size()==1) {
        auto body=text.substr(10,text.size()-11);
        Pattern result;
        if(body.size()>1 && body[1]=='|') {
            auto axis=std::string_view("xyz").find(body[0]);
            if(axis==std::string_view::npos) { error=tr("pattern.bad_syntax",text); return std::nullopt; }
            result.mAxis=int(axis);
            body.remove_prefix(2);
        }
        for(auto entry:splitTopLevel(body,[](char c){return c==',';})) {
            auto child=parse(entry,player,error);
            if(!child) return std::nullopt;
            result.mChildren.push_back(std::move(*child));
        }
        return result;
    }
    if(text.starts_with("^[") && text.ends_with(']') && splitTopLevel(text,[](char c){return c==',';}).size()==1) {
        auto states=parseStates(text.substr(2,text.size()-3));
        if(!states || states->empty()) { error=tr("pattern.bad_syntax",text); return std::nullopt; }
        Pattern result;
        result.mModify=[states=std::move(*states)](Block const& current) -> Block const& {
            auto tag=*current.mSerializationId;
            if(!tag.contains("states")) return current;
            auto& target=tag["states"].get<CompoundTag>();
            for(auto const& [key,value]:states) {
                if(!target.contains(key)) return current;
                std::visit([&](auto const& item){target[key]=item;},value);
            }
            auto block=Block::tryGetFromRegistry(tag);
            return block && !isDisallowed(*block) ? *block : current;
        };
        return result;
    }
    if (trim(text).empty()) {
        error = tr("pattern.empty");
        return std::nullopt;
    }
    std::vector<Pattern> choices;
    std::vector<double>       weights;
    double                    assigned   = 0;
    size_t                    unweighted = 0;
    for (auto entry : splitTopLevel(text, [](char c) { return c == ','; })) {
        entry          = trim(entry);
        double weight  = -1;
        auto   percent = entry.find('%');
        if (percent != std::string_view::npos && entry.find('[') > percent) {
            auto number    = entry.substr(0, percent);
            auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), weight);
            if (ec != std::errc{} || end != number.data() + number.size() || !std::isfinite(weight) || weight <= 0) {
                error = tr("pattern.bad_weight", entry);
                return std::nullopt;
            }
            entry     = entry.substr(percent + 1);
            assigned += weight;
        } else {
            ++unweighted;
        }
        if(entry.starts_with('#') || entry.starts_with('^')) {
            if(entry==text) {error=tr("pattern.bad_syntax",text);return std::nullopt;}
            auto child=parse(entry,player,error);
            if(!child) return std::nullopt;
            choices.push_back(std::move(*child));
        } else {
            auto* block=parseBlock(entry,player,error);
            if(!block) return std::nullopt;
            if(isDisallowed(*block)) {error=tr("pattern.disallowed",block->getTypeName());return std::nullopt;}
            choices.push_back(single(*block));
        }
        weights.push_back(weight);
    }

    // 未写百分比的条目平分剩余比例；百分比已满 100 时按 1 份计。
    double share = unweighted ? std::max(100.0 - assigned, 0.0) / double(unweighted) : 0;
    if (share <= 0) share = 1;
    Pattern pattern;
    double  total = 0;
    for (size_t i = 0; i < choices.size(); ++i) {
        total += weights[i] < 0 ? share : weights[i];
        pattern.mChoices.push_back(std::move(choices[i]));
        pattern.mCumulative.push_back(total);
    }
    return pattern;
}

Pattern Pattern::single(Block const& block) {
    Pattern pattern;
    pattern.mBlocks.push_back(&block);
    pattern.mCumulative.push_back(1);
    return pattern;
}

bool Pattern::isAllowed() const {
    if(mClipboard) for(size_t i=0;i<mClipboard->volume();++i) {
        auto block=mClipboard->at(i).block;
        if(block && isDisallowed(*block)) return false;
    }
    return std::ranges::none_of(mBlocks, [](Block const* block) { return isDisallowed(*block); })
        && std::ranges::all_of(mChildren,[](Pattern const& child){return child.isAllowed();})
        && std::ranges::all_of(mChoices,[](Pattern const& child){return child.isAllowed();});
}

Block const& Pattern::apply(std::mt19937& random) const {
    return apply(random,{0,0,0},{{0,0,0},{0,0,0}},airBlock());
}

Block const& Pattern::apply(std::mt19937& random, core::Vec3i position, core::Box const& bounds, Block const& current, bool* preserveData, bool* skip) const {
    if(preserveData) *preserveData=false;
    if(skip) *skip=false;
    if(mModify) {
        auto const& result=mModify(current);
        if(preserveData) *preserveData=true;
        if(skip) *skip=&result==&current;
        return result;
    }
    if(mClipboard) {
        auto wrap=[](int n,int size){int r=n%size;return r<0 ? r+size : r;};
        auto local=position-bounds.min;
        local={wrap(local.x,mClipboard->size.x),wrap(local.y,mClipboard->size.y),wrap(local.z,mClipboard->size.z)};
        auto block=mClipboard->at(mClipboard->index(local)).block;
        if(skip) *skip=!block;
        return block ? *block : current;
    }
    if(!mChildren.empty()) {
        int p=mAxis==0 ? position.x : mAxis==1 ? position.y : position.z;
        int low=mAxis==0 ? bounds.min.x : mAxis==1 ? bounds.min.y : bounds.min.z;
        int high=mAxis==0 ? bounds.max.x : mAxis==1 ? bounds.max.y : bounds.max.z;
        double t=high==low ? 0 : std::clamp(double(p-low)/(high-low),0.0,1.0);
        double stage=t*double(mChildren.size()-1);
        size_t index=size_t(stage);
        if(index+1<mChildren.size() && std::uniform_real_distribution<double>(0,1)(random)<stage-double(index)) ++index;
        return mChildren[index].apply(random,position,bounds,current,preserveData,skip);
    }
    if(!mChoices.empty()) {
        if(mChoices.size()==1) return mChoices.front().apply(random,position,bounds,current,preserveData,skip);
        double roll=std::uniform_real_distribution<double>(0,mCumulative.back())(random);
        auto it=std::ranges::upper_bound(mCumulative,roll);
        return mChoices[std::min<size_t>(it-mCumulative.begin(),mChoices.size()-1)].apply(random,position,bounds,current,preserveData,skip);
    }
    if (mBlocks.size() == 1) return *mBlocks.front();
    double roll = std::uniform_real_distribution<double>(0, mCumulative.back())(random);
    auto   it   = std::ranges::upper_bound(mCumulative, roll);
    return *mBlocks[std::min<size_t>(it - mCumulative.begin(), mBlocks.size() - 1)];
}

std::optional<Mask> Mask::parse(std::string_view text, Player const& player, std::string& error) {
    Mask mask;
    for (auto token : splitTokens(text)) {
        Term term;
        if (token.starts_with('!')) { term.negate=true; token.remove_prefix(1); }
        if(token.empty()) {error=tr("mask.empty");return std::nullopt;}
        if(token.starts_with('>') || token.starts_with('<')) {
            term.kind=token.front()=='>' ? Kind::above : Kind::below;
            auto adjacent=parse(token.substr(1),player,error);
            if(!adjacent) return std::nullopt;
            term.adjacent=std::make_shared<Mask>(std::move(*adjacent));
        } else if(token.starts_with('%')) {
            term.kind=Kind::chance;
            auto number=token.substr(1);
            auto [end,ec]=std::from_chars(number.data(),number.data()+number.size(),term.chance);
            if(ec!=std::errc{} || end!=number.data()+number.size() || !std::isfinite(term.chance) || term.chance<0 || term.chance>100) {
                error=tr("mask.bad_chance",token);return std::nullopt;
            }
        } else if(token.starts_with('$')) {
            term.kind=Kind::biome;
            term.value=std::string(token.substr(1));
            if(term.value.empty()) {
                auto p=player.getFeetBlockPos();
                term.value=player.getDimensionBlockSource().getBiome(p).mHash->getString();
            }
            term.value=normalizeName(term.value);
        } else if(token.starts_with("#tag:")) {
            term.kind=Kind::tag;
            term.value=std::string(token.substr(5));
            if(term.value.empty()) {error=tr("mask.unknown",token);return std::nullopt;}
        } else if(token.starts_with('#')) {
            if(token=="#existing") term.kind=Kind::existing;
            else if(token=="#solid") term.kind=Kind::solid;
            else if(token=="#air") term.kind=Kind::air;
            else if(token=="#surface") term.kind=Kind::surface;
            else {error=tr("mask.unknown",token);return std::nullopt;}
        } else {
            for(auto entry:splitTopLevel(token,[](char c){return c==',';})) {
                auto parsed=parseBlockSpec(entry,player,error);
                if(!parsed.block) return std::nullopt;
                term.matchers.push_back({parsed.block->getTypeName(),parsed.hasState ? parsed.block : nullptr});
            }
        }
        mask.mTerms.push_back(std::move(term));
    }
    if(mask.mTerms.empty()) {error=tr("mask.empty");return std::nullopt;}
    return mask;
}

bool Mask::needsNeighbors() const {
    return std::ranges::any_of(mTerms,[](Term const& term) {
        return term.kind==Kind::surface || (term.adjacent && term.adjacent->needsNeighbors());
    });
}

bool Mask::test(Block const& block) const { return testAt(block,nullptr,{0,0,0}); }
bool Mask::test(Block const& block, BlockSource& region, core::Vec3i position) const { return testAt(block,&region,position); }

bool Mask::testAt(Block const& block, BlockSource* region, core::Vec3i p) const {
    for(auto const& term:mTerms) {
        bool matched=false;
        switch(term.kind) {
        case Kind::existing: matched=!block.isAir(); break;
        case Kind::solid: matched=block._isSolid(); break;
        case Kind::air: matched=block.isAir(); break;
        case Kind::tag: matched=block.hasTag(HashedString(term.value)); break;
        case Kind::chance: {
            static thread_local std::mt19937 random{std::random_device{}()};
            matched=std::uniform_real_distribution<double>(0,100)(random)<term.chance;
            break;
        }
        case Kind::biome:
            matched=region && normalizeName(region->getBiome({p.x,p.y,p.z}).mHash->getString())==term.value;
            break;
        case Kind::above:
        case Kind::below:
            if(region) {
                auto adjacent=p+core::Vec3i{0,term.kind==Kind::above ? -1 : 1,0};
                matched=term.adjacent->test(region->getBlock({adjacent.x,adjacent.y,adjacent.z}),*region,adjacent);
            }
            break;
        case Kind::surface:
            if(region && !block.isAir()) {
                for(auto offset:{core::Vec3i{1,0,0},core::Vec3i{-1,0,0},core::Vec3i{0,1,0},core::Vec3i{0,-1,0},core::Vec3i{0,0,1},core::Vec3i{0,0,-1}}) {
                    auto q=p+offset;
                    if(region->getBlock({q.x,q.y,q.z}).isAir()) {matched=true;break;}
                }
            }
            break;
        case Kind::blocks:
            matched=std::ranges::any_of(term.matchers,[&](Matcher const& matcher){
                return matcher.exact ? matcher.exact==&block : matcher.typeName==block.getTypeName();
            });
            break;
        }
        if(matched==term.negate) return false;
    }
    return true;
}

} // namespace creatorx::edit
