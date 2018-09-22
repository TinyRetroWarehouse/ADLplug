//          Copyright Jean Pierre Cimalando 2018.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include "configuration.h"
#include <SimpleIni.h>

#if 0
#   define trace(fmt, ...)
#else
#   define trace(fmt, ...) fprintf(stderr, "[Configuration] " fmt "\n", ##__VA_ARGS__)
#endif

struct Configuration::Opaque_Ini {
    CSimpleIniA instance;
};

Configuration::Configuration()
    : ini_(new Opaque_Ini)
{
}

Configuration::~Configuration()
{
}

File Configuration::system_file_path()
{
    File file;
#if JUCE_LINUX
    file = File("/etc/" JucePlugin_Name "/" JucePlugin_Name ".ini");
#endif
    return file;
}

File Configuration::user_file_path()
{
    File data_dir = File::getSpecialLocation(File::userApplicationDataDirectory);
    return data_dir.getChildFile(JucePlugin_Manufacturer "/" JucePlugin_Name ".ini");
}

bool Configuration::load_default()
{
    File user = user_file_path();
    trace("Attempt to load '%s'", user.getFullPathName().toRawUTF8());
    if (user.existsAsFile() && load_file(user))
        return true;

    File sys = system_file_path();
    trace("Attempt to load '%s'", sys.getFullPathName().toRawUTF8());
    if (sys.existsAsFile() && load_file(sys))
        return true;

    init_default_contents();

    return false;
}

bool Configuration::save_default()
{
    File user = user_file_path();
    trace("Attempt to save '%s'", user.getFullPathName().toRawUTF8());
    return save_file(user);
}

bool Configuration::load_file(const File &file)
{
    return ini_->instance.LoadFile(file.getFullPathName().toRawUTF8()) == SI_OK;
}

bool Configuration::save_file(const File &file)
{
    file.getParentDirectory().createDirectory();
    return ini_->instance.SaveFile(file.getFullPathName().toRawUTF8()) == SI_OK;
}

void Configuration::set_string(const char *section, const char *key, const char *value)
{
    ini_->instance.SetValue(section, key, value, nullptr, true);
}

const char *Configuration::get_string(const char *section, const char *key, const char *default_value) const
{
    return ini_->instance.GetValue(section, key, default_value);
}

void Configuration::init_default_contents()
{
    CSimpleIniA &ini = ini_->instance;
    if(!ini.GetValue("piano", "layout"))
        ini.SetValue("piano", "layout", "qwerty", "# the default key layout");
    if(!ini.GetValue("piano", "keymap:qwerty"))
        ini.SetValue("piano", "keymap:qwerty", "awsedftgyhujkolp;", "# the QWERTY key map");
    if(!ini.GetValue("piano", "keymap:azerty"))
        ini.SetValue("piano", "keymap:azerty", "qzsedftgyhujkolpm", "# the AZERTY key map");
}