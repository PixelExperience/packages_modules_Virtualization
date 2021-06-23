/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/result.h>
#include <com_android_apex.h>
#include <image_aggregator.h>
#include <json/json.h>

#include "microdroid/metadata.h"

using android::base::Dirname;
using android::base::ErrnoError;
using android::base::Error;
using android::base::Result;
using android::base::unique_fd;
using android::microdroid::ApexPayload;
using android::microdroid::ApkPayload;
using android::microdroid::Metadata;
using android::microdroid::WriteMetadata;

using com::android::apex::ApexInfoList;
using com::android::apex::readApexInfoList;

using cuttlefish::AlignToPartitionSize;
using cuttlefish::CreateCompositeDisk;
using cuttlefish::kLinuxFilesystem;
using cuttlefish::MultipleImagePartition;

Result<uint32_t> GetFileSize(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) == -1) {
        return ErrnoError() << "Can't lstat " << path;
    }
    return static_cast<uint32_t>(st.st_size);
}

std::string ToAbsolute(const std::string& path, const std::string& dirname) {
    bool is_absolute = !path.empty() && path[0] == '/';
    if (is_absolute) {
        return path;
    } else {
        return dirname + "/" + path;
    }
}

// Returns `append` is appended to the end of filename preserving the extension.
std::string AppendFileName(const std::string& filename, const std::string& append) {
    size_t pos = filename.find_last_of('.');
    if (pos == std::string::npos) {
        return filename + append;
    } else {
        return filename.substr(0, pos) + append + filename.substr(pos);
    }
}

struct ApexConfig {
    std::string name; // the apex name
    std::string path; // the path to the apex file
                      // absolute or relative to the config file
    std::optional<std::string> public_key;
    std::optional<std::string> root_digest;
};

struct ApkConfig {
    std::string name;
    std::string path;
    // TODO(jooyung) make this required?
    std::optional<std::string> idsig_path;
};

struct Config {
    std::string dirname; // config file's direname to resolve relative paths in the config

    // TODO(b/185956069) remove this when VirtualizationService can provide apex paths
    std::vector<std::string> system_apexes;

    std::vector<ApexConfig> apexes;
    std::optional<ApkConfig> apk;
    std::optional<std::string> payload_config_path;
};

#define DO(expr) \
    if (auto res = (expr); !res.ok()) return res.error()

Result<void> ParseJson(const Json::Value& value, std::string& s) {
    if (!value.isString()) {
        return Error() << "should be a string: " << value;
    }
    s = value.asString();
    return {};
}

template <typename T>
Result<void> ParseJson(const Json::Value& value, std::optional<T>& s) {
    if (value.isNull()) {
        s.reset();
        return {};
    }
    s.emplace();
    return ParseJson(value, *s);
}

template <typename T>
Result<void> ParseJson(const Json::Value& values, std::vector<T>& parsed) {
    for (const Json::Value& value : values) {
        T t;
        DO(ParseJson(value, t));
        parsed.push_back(std::move(t));
    }
    return {};
}

Result<void> ParseJson(const Json::Value& value, ApexConfig& apex_config) {
    DO(ParseJson(value["name"], apex_config.name));
    DO(ParseJson(value["path"], apex_config.path));
    DO(ParseJson(value["publicKey"], apex_config.public_key));
    DO(ParseJson(value["rootDigest"], apex_config.root_digest));
    return {};
}

Result<void> ParseJson(const Json::Value& value, ApkConfig& apk_config) {
    DO(ParseJson(value["name"], apk_config.name));
    DO(ParseJson(value["path"], apk_config.path));
    DO(ParseJson(value["idsig_path"], apk_config.idsig_path));
    return {};
}

Result<void> ParseJson(const Json::Value& value, Config& config) {
    DO(ParseJson(value["system_apexes"], config.system_apexes));
    DO(ParseJson(value["apexes"], config.apexes));
    DO(ParseJson(value["apk"], config.apk));
    DO(ParseJson(value["payload_config_path"], config.payload_config_path));
    return {};
}

Result<Config> LoadConfig(const std::string& config_file) {
    std::ifstream in(config_file);
    Json::CharReaderBuilder builder;
    Json::Value root;
    Json::String errs;
    if (!parseFromStream(builder, in, &root, &errs)) {
        return Error() << "bad config: " << errs;
    }

    Config config;
    config.dirname = Dirname(config_file);
    DO(ParseJson(root, config));
    return config;
}

#undef DO

Result<void> LoadSystemApexes(Config& config) {
    static const char* kApexInfoListFile = "/apex/apex-info-list.xml";
    std::optional<ApexInfoList> apex_info_list = readApexInfoList(kApexInfoListFile);
    if (!apex_info_list.has_value()) {
        return Error() << "Failed to read " << kApexInfoListFile;
    }
    auto get_apex_path = [&](const std::string& apex_name) -> std::optional<std::string> {
        for (const auto& apex_info : apex_info_list->getApexInfo()) {
            if (apex_info.getIsActive() && apex_info.getModuleName() == apex_name) {
                return apex_info.getModulePath();
            }
        }
        return std::nullopt;
    };
    for (const auto& apex_name : config.system_apexes) {
        const auto& apex_path = get_apex_path(apex_name);
        if (!apex_path.has_value()) {
            return Error() << "Can't find the system apex: " << apex_name;
        }
        config.apexes.push_back(ApexConfig{
                .name = apex_name,
                .path = *apex_path,
                .public_key = std::nullopt,
                .root_digest = std::nullopt,
        });
    }
    return {};
}

Result<void> MakeMetadata(const Config& config, const std::string& filename) {
    Metadata metadata;
    metadata.set_version(1);

    for (const auto& apex_config : config.apexes) {
        auto* apex = metadata.add_apexes();

        // name
        apex->set_name(apex_config.name);

        // publicKey
        if (apex_config.public_key.has_value()) {
            apex->set_publickey(apex_config.public_key.value());
        }

        // rootDigest
        if (apex_config.root_digest.has_value()) {
            apex->set_rootdigest(apex_config.root_digest.value());
        }
    }

    if (config.apk.has_value()) {
        auto* apk = metadata.mutable_apk();
        apk->set_name(config.apk->name);
        apk->set_payload_partition_name("microdroid-apk");
        if (config.apk->idsig_path.has_value()) {
            apk->set_idsig_partition_name("microdroid-apk-idsig");
        }
    }

    if (config.payload_config_path.has_value()) {
        *metadata.mutable_payload_config_path() = config.payload_config_path.value();
    }

    std::ofstream out(filename);
    return WriteMetadata(metadata, out);
}

// fill (zeros + original file's size) with aligning BLOCK_SIZE(4096) boundary
// return true when the filler is generated.
Result<bool> SizeFiller(const std::string& file_path, const std::string& filler_path) {
    auto file_size = GetFileSize(file_path);
    if (!file_size.ok()) {
        return file_size.error();
    }
    auto disk_size = AlignToPartitionSize(*file_size + sizeof(uint32_t));

    unique_fd fd(TEMP_FAILURE_RETRY(open(filler_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600)));
    if (fd.get() == -1) {
        return ErrnoError() << "open(" << filler_path << ") failed.";
    }
    uint32_t size = htobe32(static_cast<uint32_t>(*file_size));
    if (ftruncate(fd.get(), disk_size - *file_size) == -1) {
        return ErrnoError() << "ftruncate(" << filler_path << ") failed.";
    }
    if (lseek(fd.get(), -sizeof(size), SEEK_END) == -1) {
        return ErrnoError() << "lseek(" << filler_path << ") failed.";
    }
    if (write(fd.get(), &size, sizeof(size)) <= 0) {
        return ErrnoError() << "write(" << filler_path << ") failed.";
    }
    return true;
}

// fill zeros to align |file_path|'s size to BLOCK_SIZE(4096) boundary.
// return true when the filler is generated.
Result<bool> ZeroFiller(const std::string& file_path, const std::string& filler_path) {
    auto file_size = GetFileSize(file_path);
    if (!file_size.ok()) {
        return file_size.error();
    }
    auto disk_size = AlignToPartitionSize(*file_size);
    if (disk_size <= *file_size) {
        return false;
    }
    unique_fd fd(TEMP_FAILURE_RETRY(open(filler_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600)));
    if (fd.get() == -1) {
        return ErrnoError() << "open(" << filler_path << ") failed.";
    }
    if (ftruncate(fd.get(), disk_size - *file_size) == -1) {
        return ErrnoError() << "ftruncate(" << filler_path << ") failed.";
    }
    return true;
}

// Do not generate any fillers
// Note that CreateCompositeDisk() handles gaps between partitions.
Result<bool> NoFiller(const std::string& file_path, const std::string& filler_path) {
    (void)file_path;
    (void)filler_path;
    return false;
}

Result<void> MakePayload(const Config& config, const std::string& metadata_file,
                         const std::string& output_file) {
    std::vector<MultipleImagePartition> partitions;

    // put metadata at the first partition
    partitions.push_back(MultipleImagePartition{
            .label = "metadata",
            .image_file_paths = {metadata_file},
            .type = kLinuxFilesystem,
            .read_only = true,
    });

    int filler_count = 0;
    auto add_partition = [&](auto partition_name, auto file_path, auto filler) -> Result<void> {
        std::vector<std::string> image_files{file_path};

        std::string filler_path = output_file + "." + std::to_string(filler_count++);
        if (auto ret = filler(file_path, filler_path); !ret.ok()) {
            return ret.error();
        } else if (*ret) {
            image_files.push_back(filler_path);
        }
        partitions.push_back(MultipleImagePartition{
                .label = partition_name,
                .image_file_paths = image_files,
                .type = kLinuxFilesystem,
                .read_only = true,
        });
        return {};
    };

    // put apexes at the subsequent partitions with "size" filler
    for (size_t i = 0; i < config.apexes.size(); i++) {
        const auto& apex_config = config.apexes[i];
        std::string apex_path = ToAbsolute(apex_config.path, config.dirname);
        if (auto ret = add_partition("microdroid-apex-" + std::to_string(i), apex_path, SizeFiller);
            !ret.ok()) {
            return ret.error();
        }
    }
    // put apk with "zero" filler.
    // TODO(jooyung): partition name("microdroid-apk") is TBD
    if (config.apk.has_value()) {
        std::string apk_path = ToAbsolute(config.apk->path, config.dirname);
        if (auto ret = add_partition("microdroid-apk", apk_path, ZeroFiller); !ret.ok()) {
            return ret.error();
        }
        if (config.apk->idsig_path.has_value()) {
            std::string idsig_path = ToAbsolute(config.apk->idsig_path.value(), config.dirname);
            if (auto ret = add_partition("microdroid-apk-idsig", idsig_path, NoFiller); !ret.ok()) {
                return ret.error();
            }
        }
    }

    const std::string gpt_header = AppendFileName(output_file, "-header");
    const std::string gpt_footer = AppendFileName(output_file, "-footer");
    CreateCompositeDisk(partitions, gpt_header, gpt_footer, output_file);
    return {};
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <config> <output>\n";
        return 1;
    }

    auto config = LoadConfig(argv[1]);
    if (!config.ok()) {
        std::cerr << config.error() << '\n';
        return 1;
    }

    if (const auto res = LoadSystemApexes(*config); !res.ok()) {
        std::cerr << res.error() << '\n';
        return 1;
    }

    const std::string output_file(argv[2]);
    const std::string metadata_file = AppendFileName(output_file, "-metadata");

    if (const auto res = MakeMetadata(*config, metadata_file); !res.ok()) {
        std::cerr << res.error() << '\n';
        return 1;
    }
    if (const auto res = MakePayload(*config, metadata_file, output_file); !res.ok()) {
        std::cerr << res.error() << '\n';
        return 1;
    }

    return 0;
}