#include "diagnostics.h"

#include <iostream>

namespace pentest {

void Diagnostics::set_source(const std::string& file_path,
                             const std::string& source) {
  file_ = file_path;
  lines_.clear();
  std::vector<std::string>& stored = sources_[file_path];
  stored.clear();
  std::string current;
  for (char c : source) {
    if (c == '\n') {
      stored.push_back(current);
      current.clear();
    } else if (c != '\r') {
      current.push_back(c);
    }
  }
  stored.push_back(current);
  lines_ = stored;
}

void Diagnostics::error(SourceLoc loc, const std::string& message) {
  ++count_;
  std::string file = loc.file.empty() ? file_ : loc.file;
  std::string prefix = file.empty() ? std::string() : file + ":";
  std::cerr << prefix << loc.line << ":" << loc.col << ": error: " << message
            << "\n";
  const std::vector<std::string>* lines = &lines_;
  auto it = sources_.find(file);
  if (it != sources_.end()) lines = &it->second;
  if (loc.line >= 1 && loc.line <= static_cast<int>(lines->size())) {
    const std::string& src_line = (*lines)[loc.line - 1];
    std::cerr << "  " << src_line << "\n  ";
    for (int i = 1; i < loc.col; ++i) std::cerr << ' ';
    std::cerr << "^\n";
  }
}

void Diagnostics::error(const std::string& message) {
  ++count_;
  if (file_.empty()) {
    std::cerr << "error: " << message << "\n";
  } else {
    std::cerr << file_ << ": error: " << message << "\n";
  }
}

}  // namespace pentest
