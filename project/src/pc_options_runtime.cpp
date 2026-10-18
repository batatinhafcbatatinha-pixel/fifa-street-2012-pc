#include "pc_options.h"
#include <rex/cvar.h>

namespace street4::pc {
bool ApplyOptionsToRuntime(std::string& failed_flags) {
  failed_flags.clear();
  // Preserve normal SDK / command line settings until the user saves this menu.
  std::error_code error;
  if (!std::filesystem::is_regular_file(OptionsPath(), error)) return true;
  const auto options = ReadOptions(OptionsPath());
  auto apply = [&](const char* name, const std::string& value) {
    if (!rex::cvar::SetFlagByName(name, value)) {
      if (!failed_flags.empty()) failed_flags += ", ";
      failed_flags += name;
    }
  };
  for (const auto& [name, value] : RuntimeFlagValues(options)) apply(name, value);
  return failed_flags.empty();
}
}  // namespace street4::pc
