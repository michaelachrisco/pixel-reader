#include "./filesystem.h"

#include <dirent.h>
#include <algorithm>
#include <unistd.h>

std::string get_cwd()
{
    char *cwd_buffer = get_current_dir_name();
    std::string cwd = cwd_buffer;
    free(cwd_buffer);
    return cwd;
}

static std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

std::vector<FSEntry> directory_listing(const std::string& path)
{
    DIR* dir = opendir(path.c_str());
    if (dir == NULL) {
        return {};
    }

    std::vector<FSEntry> entries;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {

        if (entry && entry->d_name)
        {
            bool is_dir = entry->d_type == DT_DIR;
            bool is_file = entry->d_type == DT_REG;
            std::string name = std::string(entry->d_name);
            if ((is_dir || is_file) && name != "." && name != "..")
            {
                entries.push_back({name, is_dir});
            }
        }
    }

    closedir(dir);

    std::sort(entries.begin(), entries.end(), [](const FSEntry& a, const FSEntry& b) {
        if (a.is_dir && !b.is_dir) {
            return true;
        }
        return to_lower(a.name) < to_lower(b.name);
    });

    return entries;
}