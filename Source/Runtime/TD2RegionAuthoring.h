#pragma once

#include "Rules/TD2Rules.h"
#include <string_view>

namespace TD2 {

// 作者の簡潔な行形式を純粋ルール定義へ変換する。装置の判定や状態更新は行わない。
bool ParseDevices(std::string_view text, std::vector<Rules::DeviceDefinition>& devices, std::string& error);

} // namespace TD2
