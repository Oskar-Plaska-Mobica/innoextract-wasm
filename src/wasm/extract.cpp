#include "extract.hpp"

// #include <zip.h>

#include <iostream>
#include <algorithm>

#include "cli/goggalaxy.hpp"
#include "emjs.h"
#include "setup/data.hpp"
#include "setup/directory.hpp"
#include "setup/filename.hpp"
#include "setup/language.hpp"
#include "stream/slice.hpp"
#include "util/load.hpp"
#include "util/log.hpp"
#include "util/time.hpp"
#include "setup/expression.hpp"

#include "wasm/fdzipstream/fdzipstream.h"

using json = nlohmann::ordered_json;

namespace wasm {

file_output::file_output(const fs::path& dir, const processed_file* f, bool write, ZIPstream *zip)
    : path_(dir / f->path()),
      file_(f),
      checksum_(f->entry().checksum.type),
      checksum_position_(f->entry().checksum.type == crypto::None ? boost::uint64_t(-1) : 0),
      position_(0),
      total_written_(0),
      write_(write),
      zip_(zip),
      file_open_(false) {
  if (write_) {
    try {
      std::ios_base::openmode flags =
          std::ios_base::out | std::ios_base::binary | std::ios_base::trunc;
      if (file_->is_multipart()) {
        flags |= std::ios_base::in;
      }
      if (zip_ == NULL) {
        throw std::exception();
      }
    } catch (...) {
      throw std::runtime_error("Could not open output file \"" + path_.string() + '"');
    }
  }
}

bool file_output::write(char* data, size_t n) {
  ZIPentry* ze;
  if (!file_open_) {
    printf("Unpacking file %s\n", path_.c_str());
    emjs::ui_innerhtml("status", path_.c_str());
    zip_entry_ = zs_entrybegin(zip_, path_.c_str(), time(0), ZS_STORE, 0);
    ze = zs_entrydata(zip_, zip_entry_, reinterpret_cast<uint8_t*>(data), n, 0);
		file_open_ = true;
	} else {
    ze = zs_entrydata(zip_, zip_entry_, reinterpret_cast<uint8_t*>(data), n, 0);
  }

  if (checksum_position_ == position_) {
    checksum_.update(data, n);
    checksum_position_ += n;
  }

  position_ += n;
  total_written_ += n;

  if (is_complete()) {
    zs_entryend(zip_, zip_entry_, 0);
  }

  return ze == zip_entry_;
}

void file_output::seek(boost::uint64_t new_position) {
  if (new_position == position_) {
    return;
  }

  printf("seek() stub! pos=%llu, newpos=%llu\n", position_, new_position);
}

void file_output::close() {
  return;
}

void file_output::settime(time_t t){
  if (file_open_) {
    zs_entrydatetime(zip_entry_, t);
  }
}

bool file_output::is_complete() const { return total_written_ == file_->entry().size; }

bool file_output::has_checksum() const { return checksum_position_ == file_->entry().size; }

bool file_output::calculate_checksum() {
  if (has_checksum()) {
    return true;
  }

  if (!write_) {
    return false;
  }

  const boost::uint64_t max =
      boost::uint64_t(std::numeric_limits<util::fstream::off_type>::max() / 4);

  boost::uint64_t diff = checksum_position_;
  stream_.seekg(util::fstream::off_type(std::min(diff, max)), std::ios_base::beg);
  diff -= std::min(diff, max);
  while (diff > 0) {
    stream_.seekg(util::fstream::off_type(std::min(diff, max)), std::ios_base::cur);
    diff -= std::min(diff, max);
  }

  while (!stream_.eof()) {
    char buffer[8192];
    std::streamsize n = stream_.read(buffer, sizeof(buffer)).gcount();
    checksum_.update(buffer, size_t(n));
    checksum_position_ += boost::uint64_t(n);
  }

  if (!has_checksum()) {
    return false;
  }

  return true;
}

crypto::checksum file_output::checksum() { return checksum_.finalize(); }

extractor& extractor::get() {
  std::call_once(init_instance_flag, &extractor::init_singleton);

  return *singleton_instance;
}

std::string extractor::load_exe(const std::string& exe_path) {
  installer_path_ = fs::path(exe_path);

  emjs::ui_progbar_update(0);

  try {
    open_installer_stream();
    load_installer_data();
  }
  catch (const setup::version_error&) {
    if (installer_offsets_.found_magic) {
      if (installer_offsets_.header_offset == 0) {
        return error_obj("Could not determine the location of setup headers!");
      } else {
        return error_obj("Could not determine setup data version!");
      }
    }
    return error_obj("Not a supported Inno Setup installer!");
  }
  catch (const std::exception& e) {
    return error_obj(e.what());
  }

  return dump_installer_info();
}

void extractor::open_installer_stream() {
  if (installer_ifs_.is_open()) {
    installer_ifs_.close();
  }

  log_info << "Opening \"" << installer_path_.string() << '"';
  installer_ifs_.open(installer_path_, std::ios_base::in | std::ios_base::binary);

  if (!installer_ifs_.is_open()) {
    throw std::runtime_error("Could not open the file \"" + installer_path_.string() + '"');
  }
}

void extractor::load_installer_data() {
  setup::info::entry_types entries = 0;
  entries |= setup::info::Files;
  entries |= setup::info::Directories;
  entries |= setup::info::DataEntries;
  entries |= setup::info::Languages;

  installer_offsets_.load(installer_ifs_);
  installer_ifs_.seekg(installer_offsets_.header_offset);
  installer_info_.load(installer_ifs_, entries);

  gog::parse_galaxy_files(installer_info_, false);
}

std::string extractor::dump_installer_info() const {
  json installer_info_obj{};
  installer_info_obj["name"] = installer_info_.header.app_versioned_name.empty()
                              ? installer_info_.header.app_name
                              : installer_info_.header.app_versioned_name;
  installer_info_obj["copyrights"] = installer_info_.header.app_copyright;
  installer_info_obj["langs"] = json::array();
  installer_info_obj["size"] = get_size() / 1024 / 1024;
  installer_info_obj["files_num"] = installer_info_.files.size();

  for (const auto& language : installer_info_.languages) {
    json lang_obj;
    lang_obj["code"] = language.name;
    lang_obj["name"] = language.language_name;
    installer_info_obj["langs"].push_back(lang_obj);
  }
  return installer_info_obj.dump();
}

std::string extractor::list_files() {
  clear_files_list();
  fetch_files();

  std::map<std::string, json::object_t*> dir_objs;
  auto main_dir_obj = create_main_dir_obj(dir_objs);

  return dump_dirs_info(main_dir_obj, dir_objs);
}

void extractor::clear_files_list() {
  dirs_.clear();
  all_files_.clear();
  all_files_.reserve(installer_info_.files.size());
}

static void add_dirs(std::set<std::string>& dirs, const std::string& path) {
  const auto path_sep_pos = path.find_last_of(setup::path_sep);
  if (path_sep_pos == std::string::npos) {
    return;
  }

  const auto dir = path.substr(0, path_sep_pos);
  dirs.insert(dir);

  add_dirs(dirs, dir);
}

void extractor::fetch_files() {
  setup::filename_map name_converter;
  name_converter.set_expand(true);

  for (const auto& directory : installer_info_.directories) {
    const std::string path = name_converter.convert(directory.name);
    if (path.empty()) {
      continue;
    }

    dirs_.insert(path);
    add_dirs(dirs_, path);
  }

  for (const auto& file : installer_info_.files) {
    if (file.location >= installer_info_.data_entries.size()) {
      continue;  // Ignore external files (copy commands)
    }

    const std::string path = name_converter.convert(file.destination);
    if (path.empty()) {
      continue;
    }

    add_dirs(dirs_, path);
    all_files_.push_back(processed_file(&file, path));
  }
}

json extractor::create_main_dir_obj(std::map<std::string, json::object_t*>& dir_objs) const {
  json main_dir;
  main_dir["text"] = installer_info_.header.app_name;
  main_dir["mainDir"] = true;
  main_dir["nodes"] = json::array();

  for (const auto& path : dirs_) {
    const size_t sep_pos = path.find_last_of(setup::path_sep);
    if (sep_pos == std::string::npos) {
      dir_objs[path] = main_dir["nodes"].emplace_back(json{{"text", path}}).get_ptr<json::object_t*>();
    } else {
      json::object_t* const parent = dir_objs[path.substr(0, sep_pos)];
      if (!parent->count("nodes")) {
        parent->emplace("nodes", json::array());
      }
      dir_objs[path] = parent->at("nodes")
                         .emplace_back(json{{"text", path.substr(sep_pos + 1)}})
                         .get_ptr<json::object_t*>();
    }
  }

  return main_dir;
}

std::string extractor::dump_dirs_info(json& main_dir_obj,
                                      std::map<std::string, json::object_t*>& dir_objs) const {
  uint32_t idx = 0;
  for (const auto& file : all_files_) {
    const std::string& path = file.path();
    json file_obj;
    size_t sep_pos = path.find_last_of(setup::path_sep);

    file_obj["text"] = path.substr(sep_pos + 1);
    file_obj["icon"] = "bi bi-file-earmark-fill";
    file_obj["fileId"] = idx++;
    file_obj["tags"] = json::array();
    file_obj["tags"].push_back(file.entry().languages);

    if (sep_pos != std::string::npos) {
      json::object_t* const parent = dir_objs[path.substr(0, sep_pos)];
      if (!parent->count("nodes")) {
        parent->emplace("nodes", json::array());
      }
      parent->at("nodes").push_back(file_obj);
    } else {
      main_dir_obj["nodes"].push_back(file_obj);
    }
  }

  json main_obj;
  main_obj.emplace_back(main_dir_obj);

  return main_obj.dump();
}

std::string extractor::extract(const std::string& list_json) {
  const std::string& output_dir = installer_info_.header.app_name;
  auto input = json::parse(list_json);
  auto files = input["files"];
  std::string lang;
  std::vector<const processed_file*> selected_files;
  set_abort(false);
  selected_files.reserve(all_files_.size());

  if (input.contains("lang")){
    lang = input["lang"];
  }

  std::sort(files.begin(), files.end());
  log_info << "Unpacking " << files.size() << " files have been started.";
  for (const auto& f : files) {
    selected_files.push_back(&all_files_[f]);
  }

  // cleaning MEMFS
  if (fs::exists(output_dir)) {
    fs::remove_all(output_dir);
  }

  // creating empty directories - ignoring user input
  // writing directly to the ZIP will resolve that in the future
  fs::create_directory(output_dir);

  for (const auto& dir : dirs_) {
    fs::create_directory(output_dir + "/" + dir);
  }

  std::string zipfile = output_dir + ".zip";
  emjs::open(zipfile.c_str(), "wb");
  emscripten_sleep(100);
  output_zip_stream_ = zs_init(nullptr);
  printf("opening zip file %s\n", zipfile.c_str());

  typedef std::pair<const processed_file*, uint64_t> output_location;
  std::vector<std::vector<output_location> > files_for_location;
  files_for_location.resize(installer_info_.data_entries.size());


  for (const processed_file* file_ptr : selected_files) {
    if(file_ptr->entry().languages.empty() || lang.empty() || setup::expression_match(lang, file_ptr->entry().languages))
      files_for_location[file_ptr->entry().location].push_back(output_location(file_ptr, 0));
    uint64_t offset = installer_info_.data_entries[file_ptr->entry().location].uncompressed_size;
    uint32_t sort_slice = installer_info_.data_entries[file_ptr->entry().location].chunk.first_slice;
    uint32_t sort_offset = installer_info_.data_entries[file_ptr->entry().location].chunk.sort_offset;
    for (uint32_t location : file_ptr->entry().additional_locations) {
      setup::data_entry& data = installer_info_.data_entries[location];
      files_for_location[location].push_back(output_location(file_ptr, offset));
      offset += data.uncompressed_size;
      if (data.chunk.first_slice > sort_slice ||
          (data.chunk.first_slice == sort_slice && data.chunk.sort_offset > sort_offset)) {
        sort_slice = data.chunk.first_slice;
        sort_offset = data.chunk.sort_offset;
      } else if (data.chunk.first_slice == sort_slice &&
                 data.chunk.sort_offset == data.chunk.offset) {
        data.chunk.sort_offset = ++sort_offset;
      } else {
        // Could not reorder chunk - no point in trying to reordder the
        // remaining chunks
        sort_slice = -1;
      }
    }
  }

  total_size_ = 0;

  typedef std::map<stream::file, size_t> Files;
  typedef std::map<stream::chunk, Files> Chunks;
  Chunks chunks;
  for (size_t i = 0; i < installer_info_.data_entries.size(); i++) {
    if (!files_for_location[i].empty()) {
      setup::data_entry& location = installer_info_.data_entries[i];
      chunks[location.chunk][location.file] = i;
      total_size_ += location.uncompressed_size;
    }
  }

  log_info << "Total size: " << total_size_ << " bytes";

  try {
    std::unique_ptr<stream::slice_reader> slice_reader;
    if (installer_offsets_.data_offset) {
      slice_reader.reset(new stream::slice_reader(&installer_ifs_, installer_offsets_.data_offset));
    } else {
      fs::path dir = installer_path_.parent_path();
      std::string basename = installer_path_.stem().string();
      std::string basename2 = installer_info_.header.base_filename;
      // Prevent access to unexpected files
      std::replace(basename2.begin(), basename2.end(), '/', '_');
      std::replace(basename2.begin(), basename2.end(), '\\', '_');
      // Older Inno Setup versions used the basename stored in the headers,
      // change our default accordingly
      if (installer_info_.version < INNO_VERSION(4, 1, 7) && !basename2.empty()) {
        std::swap(basename2, basename);
      }
      slice_reader.reset(
          new stream::slice_reader(dir, basename, basename2, installer_info_.header.slices_per_disk));
    }

    bytes_extracted_ = 0;
    multi_outputs_.clear();

    for (const Chunks::value_type& chunk : chunks) {  //[first = chunk, second = [file, location]]
      stream::chunk_reader::pointer chunk_source;
      if (chunk.first.encryption == stream::Plaintext) {
        chunk_source = stream::chunk_reader::get(*slice_reader, chunk.first, "");
      }
      uint64_t offset = 0;
      for (const Files::value_type& location : chunk.second) {  // 1 chunk => n files
        const stream::file& file = location.first;
        const std::vector<output_location>& output_locations = files_for_location[location.second];
        if (file.offset > offset) {
          if (chunk_source.get()) {
            util::discard(*chunk_source, file.offset - offset);
          }
        }
        if (chunk_source.get() && file.offset < offset) {
          std::ostringstream oss;
          oss << "Bad offset while extracting files: file start (" << file.offset
              << ") is before end of previous file (" << offset << ")!";
          throw std::runtime_error(oss.str());
        }
        offset = file.offset + file.size;

        if (!chunk_source.get()) {
          continue;  // Not extracting/testing this file
        }

        crypto::checksum checksum;

        // Open input file
        stream::file_reader::pointer file_source;
        file_source = stream::file_reader::get(*chunk_source, file, &checksum);

        std::vector<file_output*> outputs;
        for (const output_location& output_loc : output_locations) {  // 1 file => n output files
          const processed_file* fileinfo = output_loc.first;

          // Re-use existing file output for multi-part files
          file_output* output = NULL;
          if (fileinfo->is_multipart()) {
            multi_part_outputs::iterator it = multi_outputs_.find(fileinfo);
            if (it != multi_outputs_.end()) {
              output = it->second;
            }
          }

          if (!output) {
            output = new file_output(output_dir, fileinfo, true, output_zip_stream_);
            if (fileinfo->is_multipart()) {
              multi_outputs_.insert(fileinfo, output);
            }
          }

          outputs.push_back(output);
          output->seek(output_loc.second);
        }

        uint64_t output_size = copy_data(file_source, outputs);

        if (aborted) { // copy_data is the most likely function to be in while execution is being aborted
          log_info << "Extraction aborted";
          abort_zip();

          json ret;
          ret["status"]="Aborted by user";
          return ret.dump();
        }

        const setup::data_entry& data = installer_info_.data_entries[location.second];

        if (output_size != data.uncompressed_size) {
          log_warning << "Unexpected output file size: " << output_size
                      << " != " << data.uncompressed_size;
        }

        verify_close_outputs(outputs, data);

        if (checksum != file.checksum) {
          log_warning << "Checksum mismatch:\n"
                      << " ├─ actual:   " << checksum << '\n'
                      << " └─ expected: " << file.checksum;
        }
      }
    }
  } catch (const std::exception& e) {
    return error_obj(e.what());
  }

  if (!multi_outputs_.empty()) {
    log_warning << "Incomplete multi-part files";
  }

  log_info << "Done. Creating ZIP file.";
  save_zip();

  json ret;
  ret["status"]="Completed successfully";
  return ret.dump();
}

void extractor::init_singleton() {
  singleton_instance = new extractor();
}

const char* extractor::error_obj(const std::string& msg) {
  static std::string result;

  log_error << msg;

  json error_obj;
  error_obj["error"] = msg;
  
  result = error_obj.dump();
  return result.c_str();
}

extractor* extractor::singleton_instance = {};
std::once_flag extractor::init_instance_flag = {};

extractor::extractor() { color::init(color::disable, color::disable); }

uint64_t extractor::get_size() const {
  return std::accumulate(
    installer_info_.data_entries.cbegin(),
    installer_info_.data_entries.cend(),
    uint64_t{0},
    [](const uint64_t acc, const setup::data_entry& entry) {
      return acc + entry.uncompressed_size;
    }
  );
}

uint64_t extractor::copy_data(const stream::file_reader::pointer& source,
                              const std::vector<file_output*>& outputs) {
  uint64_t output_size = 0;
  while (!source->eof()) {
    char buffer[8192 * 10];
    const auto buffer_size = std::streamsize(boost::size(buffer));
    const auto extracted_n = source->read(buffer, buffer_size).gcount();
    if (extracted_n > 0) {
      for (auto output : outputs) {
        bool success = output->write(buffer, extracted_n);
        if (!success) {
          throw std::runtime_error("Error writing file \"" + output->path().string() + '"');
        }
      }
      bytes_extracted_ += extracted_n;
      output_size += extracted_n;

      emjs::ui_progbar_update(float(bytes_extracted_) / total_size_ * 100);
    }
    if (aborted) {
      return 0;
    }

  }

  return output_size;
}

void extractor::verify_close_outputs(const std::vector<file_output*>& outputs,
                                    const setup::data_entry& data) {
  for (const auto output : outputs) {
    if (output->file()->is_multipart() && !output->is_complete()) {
      continue;
    }
    if (output->file()->entry().checksum.type != crypto::None && output->calculate_checksum()) {
      const auto output_checksum = output->checksum();
      if (output_checksum != output->file()->entry().checksum) {
        log_warning << "Output checksum mismatch for " << output->file()->path() << ":\n"
                    << " ├─ actual:   " << output_checksum << '\n'
                    << " └─ expected: " << output->file()->entry().checksum;
      }
    }
    output->close();
    output->settime(data.timestamp);

    if (output->file()->is_multipart()) {
      multi_outputs_.erase(output->file());
    }
  }
}

void extractor::save_zip() {
  zs_finish(output_zip_stream_, 0);
  zs_free(output_zip_stream_);

  emjs::write(nullptr, 0, 0);
  emjs::close();
}

void extractor::abort_zip() {
  zs_free(output_zip_stream_);
  emjs::abort_down();
}

void extractor::set_abort(bool state) {
  aborted = state;
}

}  // namespace wasm