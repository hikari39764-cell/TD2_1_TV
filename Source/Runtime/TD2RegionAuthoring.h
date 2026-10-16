#pragma once

#include "Rules/TD2Rules.h"
#include <string_view>

namespace TD2 {

// 作者の行形式をルール定義へ変換する。判定や状態の更新はしない。
bool ParseDevices(std::string_view text, std::vector<Rules::DeviceDefinition>& devices, std::string& error);

}
