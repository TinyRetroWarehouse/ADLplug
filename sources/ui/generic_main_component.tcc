//          Copyright Jean Pierre Cimalando 2018.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include "generic_main_component.h"
#include "plugin_processor.h"
#include "ui/components/new_program_editor.h"
#include "ui/components/program_name_editor.h"
#include "ui/components/midi_keyboard_ex.h"
#include "midi/insnames.h"
#include "utility/functional_timer.h"
#include "utility/simple_fifo.h"
#include "utility/pak.h"
#include "BinaryData.h"
#include <fmt/format.h>
#include <cassert>

#if 0
#   define trace(fmt, ...)
#else
#   define trace(fmt, ...) fprintf(stderr, "[UI Main] " fmt "\n", ##__VA_ARGS__)
#endif

#if !JUCE_LINUX
static constexpr bool prefer_native_file_dialog = true;
#else
static constexpr bool prefer_native_file_dialog = false;
#endif

template <class T>
inline T *Generic_Main_Component<T>::self()
{
    return static_cast<T *>(this);
}

template <class T>
inline const T *Generic_Main_Component<T>::self() const
{
    return static_cast<const T *>(this);
}

template <class T>
Generic_Main_Component<T>::Generic_Main_Component(
    AdlplugAudioProcessor &proc, Parameter_Block &pb, Configuration &conf)
    : proc_(&proc), parameter_block_(&pb), conf_(&conf)
{
    Desktop::getInstance().addFocusChangeListener(this);
    setWantsKeyboardFocus(true);
    midi_kb_state_.addListener(this);
    bank_directory_ = File::getSpecialLocation(File::currentExecutableFile).getParentDirectory();
}

template <class T>
Generic_Main_Component<T>::~Generic_Main_Component()
{
    Desktop::getInstance().removeFocusChangeListener(this);
}

template <class T>
void Generic_Main_Component<T>::setup_generic_components()
{
    Configuration &conf = *conf_;

    self()->last_key_layout_ = load_key_configuration(*self()->midi_kb, conf);
    self()->midi_kb->setKeyPressBaseOctave(midi_kb_octave_);
    self()->midi_kb->setLowestVisibleKey(24);

    self()->btn_bank_load->setTooltip(TRANS("Load bank"));
    self()->btn_bank_save->setTooltip(TRANS("Save bank"));
    create_image_overlay(*self()->btn_bank_load, ImageCache::getFromMemory(BinaryData::emoji_u1f4c2_png, BinaryData::emoji_u1f4c2_pngSize), 0.7f);
    create_image_overlay(*self()->btn_bank_save, ImageCache::getFromMemory(BinaryData::emoji_u1f4be_png, BinaryData::emoji_u1f4be_pngSize), 0.7f);

    create_image_overlay(*self()->btn_pgm_edit, ImageCache::getFromMemory(BinaryData::emoji_u1f4dd_png, BinaryData::emoji_u1f4dd_pngSize), 0.7f);
    create_image_overlay(*self()->btn_pgm_add, ImageCache::getFromMemory(BinaryData::emoji_u2795_png, BinaryData::emoji_u2795_pngSize), 0.7f);

    for (unsigned note = 0; note < 128; ++note) {
        const char *octave_names[12] =
            {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        String name = octave_names[note % 12] + String((int)(note / 12) - 1);
        self()->cb_percussion_key->addItem(name, note + 1);
    }
    self()->cb_percussion_key->setSelectedId(69 + 1, dontSendNotification);
    self()->cb_percussion_key->setScrollWheelEnabled(true);

    create_image_overlay(*self()->btn_keymap, ImageCache::getFromMemory(BinaryData::emoji_u2328_png, BinaryData::emoji_u2328_pngSize), 0.7f);

    vu_timer_.reset(Functional_Timer::create([this]() { vu_update(); }));
    vu_timer_->startTimer(10);

    cpu_load_timer_.reset(Functional_Timer::create([this]() { cpu_load_update(); }));
    cpu_load_timer_->startTimer(500);
    self()->lbl_cpu->setText("0%", dontSendNotification);

    midi_activity_timer_.reset(Functional_Timer::create([this]() { midi_activity_update(); }));
    midi_activity_timer_->startTimer(100);

    midi_keys_timer_.reset(Functional_Timer::create([this]() { midi_keys_update(); }));
    midi_keys_timer_->startTimer(40);
}

template <class T>
void Generic_Main_Component<T>::on_ready_processor()
{
    Messages::User::RequestChipSettings msg_chip;
    write_to_processor(msg_chip.tag, &msg_chip, sizeof(msg_chip));

    Messages::User::RequestFullBankState msg_bank;
    write_to_processor(msg_bank.tag, &msg_bank, sizeof(msg_bank));
}

template <class T>
bool Generic_Main_Component<T>::is_percussion_channel(unsigned channel) const
{
    return channel == 9;
}

template <class T>
void Generic_Main_Component<T>::send_controller(unsigned channel, unsigned ctl, unsigned value)
{
    uint8_t midi[3];
    midi[0] = (channel & 15) | (0b1011u << 4);
    midi[1] = ctl & 127;
    midi[2] = value & 127;
    write_to_processor(User_Message::Midi, midi, 3);
}

template <class T>
void Generic_Main_Component<T>::send_program_change(unsigned channel, unsigned value)
{
    uint8_t midi[2];
    midi[0] = (channel & 15) | (0b1100u << 4);
    midi[1] = value & 127;
    write_to_processor(User_Message::Midi, midi, 2);
}

template <class T>
void Generic_Main_Component<T>::send_rename_bank(Bank_Id bank, const String &name)
{
    Messages::User::RenameBank msg;
    msg.bank = bank;
    msg.notify_back = true;
    const char *utf8 = name.toRawUTF8();
    memset(msg.name, 0, 32);
    memcpy(msg.name, utf8, strnlen(utf8, 32));
    write_to_processor(msg.tag, &msg, sizeof(msg));
}

template <class T>
void Generic_Main_Component<T>::send_rename_program(Bank_Id bank, unsigned pgm, const String &name)
{
    // TODO
    
    
}

template <class T>
void Generic_Main_Component<T>::send_create_program(Bank_Id bank, unsigned pgm)
{
    Messages::User::CreateInstrument msg;
    msg.bank = bank;
    msg.program = pgm;
    msg.notify_back = true;
    write_to_processor(msg.tag, &msg, sizeof(msg));
}

template <class T>
Instrument *Generic_Main_Component<T>::find_instrument(uint32_t program, Instrument *if_not_found)
{
    uint32_t psid = program >> 8;
    auto it = instrument_map_.find(psid);
    if (it == instrument_map_.end())
        return if_not_found;
    return &it->second.ins[program & 255];
}

template <class T>
void Generic_Main_Component<T>::reload_selected_instrument(NotificationType ntf)
{
    int selection = self()->cb_program->getSelectedId();

    trace("Reload selected instrument %s", program_selection_to_string(selection).toRawUTF8());

    Instrument ins_empty, *ins = &ins_empty;
    if (selection != 0) {
        uint32_t program = (unsigned)selection - 1;
        ins = find_instrument(program, &ins_empty);
    }
    self()->set_instrument_parameters(*ins, ntf);
}

template <class T>
void Generic_Main_Component<T>::send_selection_update()
{
    int selection = self()->cb_program->getSelectedId();

    unsigned insno = 0;
    unsigned psid = 0;
    if (selection != 0) {
        insno = ((unsigned)selection - 1) & 255;
        psid = ((unsigned)selection - 1) >> 8;
        trace("Send selection update %s",
              program_selection_to_string(selection).toRawUTF8());
    }
    else {
        trace("Send selection update 0:0:0 because of empty selection");
    }

    Messages::User::SelectProgram msg;
    msg.part = midichannel_;
    msg.bank = Bank_Id(psid >> 7, psid & 127, insno >= 128);
    msg.program = insno & 127;
    write_to_processor(msg.tag, &msg, sizeof(msg));
}

template <class T>
void Generic_Main_Component<T>::receive_bank_slots(const Messages::Fx::NotifyBankSlots &msg)
{
    unsigned count = msg.count;
    bool update = false;
    auto &imap = instrument_map_;

    trace("Receive %u bank slots", count);

    // delete bank entries not in the slots
    for (auto it = imap.begin(), end = imap.end(); it != end;) {
        uint32_t psid = it->first;
        bool found = false;
        for (unsigned slotno = 0; slotno < count && !found; ++slotno)
            found = msg.entry[slotno].bank.msb == (psid >> 7) &&
                msg.entry[slotno].bank.lsb == (psid & 127);
        if (found)
            ++it;
        else {
            imap.erase(it++);
            update = true;
        }
    }

    // extract the names
    std::map<Bank_Id, std::array<char, 32>> bank_name_map;
    for (unsigned slotno = 0; slotno < count; ++slotno) {
        const Messages::Fx::NotifyBankSlots::Entry &entry = msg.entry[slotno];
        if (entry.name[0] != '\0')
            std::memcpy(bank_name_map[entry.bank].data(), entry.name, 32);
    }

    // enable or disable instruments according to slots
    for (unsigned slotno = 0; slotno < count; ++slotno) {
        const Messages::Fx::NotifyBankSlots::Entry &entry = msg.entry[slotno];
        uint32_t psid = entry.bank.pseudo_id();
        bool percussive = entry.bank.percussive;
        Editor_Bank &e_bank = imap[psid];
        for (unsigned i = 0; i < 128; ++i) {
            unsigned insno = i + (percussive ? 128 : 0);
            bool isblank = !entry.used.test(i);
            if (e_bank.ins[insno].blank() != isblank) {
                e_bank.ins[insno].blank(isblank);
                update = true;
            }
        }
        const char *name_src;
        char *name_dst = entry.bank.percussive ? e_bank.percussion_name : e_bank.melodic_name;
        auto it = bank_name_map.find(Bank_Id(entry.bank.msb, entry.bank.lsb, entry.bank.percussive));
        if (it != bank_name_map.end())
            name_src = it->second.data();
        else {
            static const char name_empty[32] = {};
            name_src = name_empty;
        }
        unsigned name_len = strnlen(name_src, 32);
        if (std::memcmp(name_dst, name_src, std::min(name_len + 1, 32u)) != 0) {
            std::memcpy(name_dst, name_src, 32);
            update = true;
        }
    }

    if (update) {
        trace("Refresh choices because of received slots");
        update_instrument_choices();
    }
}

template <class T>
void Generic_Main_Component<T>::receive_global_parameters(const Instrument_Global_Parameters &gp)
{
    trace("Receive global parameters");

    instrument_gparam_ = gp;
    self()->set_global_parameters(dontSendNotification);
}

template <class T>
void Generic_Main_Component<T>::receive_instrument(Bank_Id bank, unsigned pgm, const Instrument &ins)
{
    assert(pgm < 128);

    Editor_Bank *e_bank;
    bool update;
    unsigned insno = pgm + (bank.percussive ? 128 : 0);
    uint32_t psid = bank.pseudo_id();

    trace("Receive instrument %u:%u:%u", bank.msb, bank.lsb, insno);

    auto &instrument_map = instrument_map_;
    auto it = instrument_map.find(psid);
    if (it == instrument_map.end()) {
        if (ins.blank())
            return;
        it = instrument_map.insert({psid, Editor_Bank()}).first;
        e_bank = &it->second;
        e_bank->ins[insno] = ins;
        update = true;
    }
    else {
        e_bank = &it->second;
        update = !e_bank->ins[insno].equal_instrument(ins);
        if (update)
            e_bank->ins[insno] = ins;
    }

    bool empty_bank = ins.blank();
    for (unsigned i = 0; i < 256 && empty_bank; ++i)
        empty_bank = e_bank->ins[i].blank();

    if (empty_bank) {
        instrument_map.erase(it);
        update = true;
    }

    if (update) {
        trace("Refresh choices because of received instrument");
        update_instrument_choices();
    }
}

template <class T>
void Generic_Main_Component<T>::receive_chip_settings(const Chip_Settings &cs)
{
    trace("Receive chip settings");

    chip_settings_ = cs;
    self()->set_chip_settings(dontSendNotification);
}

template <class T>
void Generic_Main_Component<T>::receive_selection(unsigned part, Bank_Id bank, uint8_t pgm)
{
    const auto &instrument_map = instrument_map_;
    uint32_t program = pgm + (bank.percussive ? 128 : 0);

    std::array<Bank_Id, 3> fallback_ids {
        Bank_Id(bank.msb, bank.lsb, bank.percussive),
        Bank_Id(bank.msb, 0, bank.percussive),  // GS fallback
        Bank_Id(0, 0, bank.percussive),  // zero fallback
    };

    unsigned selection = 0;
    bool found = false;
    for (size_t i = 0; !found && i < fallback_ids.size(); ++i) {
        uint32_t psid = fallback_ids[i].pseudo_id();
        auto it = instrument_map.find(psid);
        if (it != instrument_map.end()) {
            found = !it->second.ins[program].blank();
            selection = (psid << 8) | program;
        }
    }

    if (!found || midiprogram_[part] == selection)
        return;
    midiprogram_[part] = selection;

    if (part == midichannel_) {
        set_program_selection(midiprogram_[part] + 1, dontSendNotification);
        reload_selected_instrument(dontSendNotification);
    }
}

template <class T>
void Generic_Main_Component<T>::update_instrument_choices()
{
    ComboBox &cb = *self()->cb_program;
    cb.clear(dontSendNotification);
    PopupMenu *menu = cb.getRootMenu();

    auto &instrument_map = instrument_map_;
    auto it = instrument_map.begin();

    for (; it != instrument_map.end(); ++it) {
        uint32_t psid = it->first;
        Editor_Bank &e_bank = it->second;

        String bank_sid;
        if (e_bank.melodic_name[0] != '\0')
            bank_sid = fmt::format("{:03d}:{:03d} {:.32s}", psid >> 7, psid & 127, e_bank.melodic_name);
        else if (e_bank.percussion_name[0] != '\0')
            bank_sid = fmt::format("{:03d}:{:03d} {:.32s}", psid >> 7, psid & 127, e_bank.percussion_name);
        else
            bank_sid = fmt::format("{:03d}:{:03d} {:s}", psid >> 7, psid & 127, "<Untitled bank>");

        e_bank.ins_menu.clear();
        for (unsigned i = 0; i < 256; ++i) {
            const Instrument &ins = e_bank.ins[i];
            if (ins.blank())
                continue;

            String ins_sid;
            if (ins.name[0] != '\0')
                ins_sid = fmt::format("{:c}{:03d} {:.32s}", "MP"[i >= 128], i & 127, ins.name);
            else {
                const Midi_Program_Ex *ex = midi_db.find_ex(psid >> 7, psid & 127, i);
                const char *name = ex ? ex->name : (i < 128) ?
                    midi_db.inst(i) : midi_db.perc(i & 127).name;
                ins_sid = fmt::format("{:c}{:03d} {:s}", "MP"[i >= 128], i & 127, name);
            }

            uint32_t program = (psid << 8) + i;
            e_bank.ins_menu.addItem(program + 1, ins_sid);

            if (false)
                trace("Add choice %s %s",
                      program_selection_to_string(program + 1).toRawUTF8(),
                      ins_sid.toRawUTF8());
        }

        menu->addSubMenu(bank_sid, e_bank.ins_menu);
    }

    unsigned channel = midichannel_;
    set_program_selection(midiprogram_[channel] + 1, dontSendNotification);
    reload_selected_instrument(dontSendNotification);
    send_selection_update();
}

template <class T>
void Generic_Main_Component<T>::set_program_selection(int selection, NotificationType ntf)
{
    trace("Change program selection %s to %s",
          program_selection_to_string(self()->cb_program->getSelectedId()).toRawUTF8(),
          program_selection_to_string(selection).toRawUTF8());

    self()->cb_program->setSelectedId(selection, ntf);

    trace("New program selection %s",
          program_selection_to_string(self()->cb_program->getSelectedId()).toRawUTF8());
}

template <class T>
String Generic_Main_Component<T>::program_selection_to_string(int selection)
{
    if (selection == 0)
        return "(nil)";

    unsigned insno = ((unsigned)selection - 1) & 255;
    unsigned psid = ((unsigned)selection - 1) >> 8;

    char buf[64];
    std::sprintf(buf, "%u:%u:%u", psid >> 7, psid & 127, insno);
    return buf;
}

template <class T>
void Generic_Main_Component<T>::handle_selected_program(int selection)
{
    trace("Select program by UI %s",
          program_selection_to_string(selection).toRawUTF8());

    if (selection != 0) {
        unsigned insno = ((unsigned)selection - 1) & 255;
        unsigned psid = ((unsigned)selection - 1) >> 8;
        unsigned channel = midichannel_;
        bool isdrum = is_percussion_channel(channel);
        midiprogram_[channel] = (psid << 8) | insno;

        if (isdrum && insno >= 128) {
            trace("Assign %s to percussion channel %u",
                  program_selection_to_string(selection).toRawUTF8(),
                  channel + 1);
            // percussion bank change LSB only
            send_program_change(channel, psid);
        }
        else if (!isdrum && insno < 128) {
            trace("Assign %s to melodic channel %u",
                  program_selection_to_string(selection).toRawUTF8(),
                  channel + 1);
            send_controller(channel, 0, psid >> 7);
            send_controller(channel, 32, psid & 127);
            send_program_change(channel, insno);
        }

        send_selection_update();
    }
    reload_selected_instrument(dontSendNotification);
}

template <class T>
void Generic_Main_Component<T>::handle_edit_program()
{
    if (dlg_edit_program_)
        return;

    DialogWindow::LaunchOptions dlgopts;
    dlgopts.dialogTitle = "Edit program";
    dlgopts.componentToCentreAround = this;
    dlgopts.resizable = false;

    unsigned part = midichannel_;
    uint32_t program = midiprogram_[part];
    uint32_t psid = program >> 8;
    bool percussive = program & 128;
    Bank_Id bank(psid >> 7, psid & 127, percussive);

    auto &instrument_map = instrument_map_;
    auto it = instrument_map.find(psid);
    if (it == instrument_map.end())
        return;

    Editor_Bank &e_bank = it->second;
    Instrument &ins = e_bank.ins[program & 255];
    if (ins.blank())
        return;

    Program_Name_Editor *editor = new Program_Name_Editor;
    dlgopts.content.set(editor, true);

    char bank_name[33], ins_name[33];
    sprintf(bank_name, "%.32s", percussive ? e_bank.percussion_name : e_bank.melodic_name);
    sprintf(ins_name, "%.32s", ins.name);
    editor->set_program(bank, program & 127, bank_name, ins_name);

    Component::SafePointer<Generic_Main_Component<T>> self(this);
    editor->on_ok = [self](const Program_Name_Editor::Result &result) {
                        if (!self || !self->dlg_edit_program_)
                            return;
                        self.getComponent()->send_rename_bank(result.bank, result.bank_name);
                        self.getComponent()->send_rename_program(result.bank, result.pgm, result.pgm_name);
                        delete self->dlg_edit_program_.getComponent();
                    };
    editor->on_cancel = [self]() {
                            if (!self || !self->dlg_edit_program_)
                                return;
                            delete self->dlg_edit_program_.getComponent();
                        };

    dlg_edit_program_ = dlgopts.launchAsync();
}

template <class T>
void Generic_Main_Component<T>::handle_add_program()
{
    PopupMenu menu;
    menu.addItem(1, "Add program");
    menu.addItem(2, "Delete program");
    int selection = menu.showMenu(PopupMenu::Options()
                                  .withParentComponent(this));

    unsigned part = midichannel_;
    uint32_t program = midiprogram_[part];
    uint32_t psid = program >> 8;
    bool percussive = program & 128;
    Bank_Id bank(psid >> 7, psid & 127, percussive);

    switch (selection) {
    case 1: {
        if (dlg_new_program_)
            return;

        DialogWindow::LaunchOptions dlgopts;
        dlgopts.dialogTitle = "Add program";
        dlgopts.componentToCentreAround = this;
        dlgopts.resizable = false;

        New_Program_Editor *editor = new New_Program_Editor;
        dlgopts.content.set(editor, true);

        

        Component::SafePointer<Generic_Main_Component<T>> self(this);
        editor->on_ok = [self](const New_Program_Editor::Result &result) {
                            if (!self || !self->dlg_new_program_)
                                return;
                            self.getComponent()->send_create_program(result.bank, result.pgm);
                            delete self->dlg_new_program_.getComponent();
                        };
        editor->on_cancel = [self]() {
                                if (!self || !self->dlg_new_program_)
                                    return;
                                delete self->dlg_new_program_.getComponent();
                            };

        dlg_new_program_ = dlgopts.launchAsync();
        break;
    }
    case 2: {
        bool confirm = AlertWindow::showOkCancelBox(
            AlertWindow::QuestionIcon, "Delete program",
            fmt::format("Confirm deletion of program {:c}{:03d}?",
                        percussive ? 'P' : 'M', program & 127));
        if (confirm) {
            Messages::User::DeleteInstrument msg;
            msg.bank = bank;
            msg.program = program & 127;
            msg.notify_back = true;
            write_to_processor(msg.tag, &msg, sizeof(msg));
        }
        break;
    }
    }
}

template <class T>
void Generic_Main_Component<T>::create_image_overlay(Component &component, Image image, float ratio)
{
    ImageComponent *overlay = new ImageComponent;
    image_overlays_.push_back(std::unique_ptr<ImageComponent>(overlay));
    Rectangle<int> bounds = component.getBounds();
    bounds = bounds.withSizeKeepingCentre(ratio * bounds.getWidth(), ratio * bounds.getHeight());
    overlay->setBounds(bounds);
    overlay->setImage(image, RectanglePlacement::centred);
    overlay->setInterceptsMouseClicks(false, true);
    addAndMakeVisible(overlay);
}

template <class T>
void Generic_Main_Component<T>::vu_update()
{
    AdlplugAudioProcessor &proc = *proc_;
    self()->vu_left->set_value(proc.vu_level(0));
    self()->vu_right->set_value(proc.vu_level(1));
}

template <class T>
void Generic_Main_Component<T>::cpu_load_update()
{
    AdlplugAudioProcessor &proc = *proc_;
    String text = String((int)(100.0 * proc.cpu_load())) + "%";
    self()->lbl_cpu->setText(text, dontSendNotification);
}

template <class T>
void Generic_Main_Component<T>::midi_activity_update()
{
    AdlplugAudioProcessor &proc = *proc_;
    for (unsigned i = 0; i < 16; ++i) {
        bool active = proc.midi_channel_note_count(i) > 0;
        unsigned columns = self()->ind_midi_activity->columns();
        self()->ind_midi_activity->set_value(i / columns, i % columns, active);
    }
}

template <class T>
void Generic_Main_Component<T>::midi_keys_update()
{
    AdlplugAudioProcessor &proc = *proc_;
    Midi_Keyboard_Ex &kb = *self()->midi_kb;
    unsigned midichannel = midichannel_;
    for (unsigned note = 0; note < 128; ++note)
        kb.highlight_note(note, proc.midi_channel_note_active(midichannel, note) ? 127 : 0);
}

template <class T>
void Generic_Main_Component<T>::update_emulator_icon()
{
    const Emulator_Defaults &defaults = get_emulator_defaults();
    unsigned emulator = chip_settings_.emulator;

    self()->btn_emulator->setImages(
        false, true, true,
        defaults.images[emulator], 1, Colour(),
        Image(), 1, Colour(),
        Image(), 1, Colour());
    self()->btn_emulator->setTooltip(defaults.choices[emulator]);
}

template <class T>
void Generic_Main_Component<T>::build_emulator_menu(PopupMenu &menu)
{
    const Emulator_Defaults &defaults = get_emulator_defaults();
    unsigned count = defaults.choices.size();

    menu.clear();
    for (size_t i = 0; i < count; ++i)
        menu.addItem(i + 1, defaults.choices[i], true, false, defaults.images[i]);
}

template <class T>
int Generic_Main_Component<T>::select_emulator_by_menu()
{
    PopupMenu menu;
    build_emulator_menu(menu);
    unsigned emulator = chip_settings_.emulator;
    int selection = menu.showMenu(PopupMenu::Options()
                                  .withParentComponent(this)
                                  .withItemThatMustBeVisible(emulator + 1));
    return selection;
}

template <class T>
void Generic_Main_Component<T>::handle_load_bank(Component *clicked)
{
#if defined(ADLPLUG_OPL3)
    const char *file_filter = "*.wopl";
#elif defined(ADLPLUG_OPN2)
    const char *file_filter = "*.wopn";
#endif

    PopupMenu menu;
    menu.addItem(1, "Load bank file...");

    Pak_File_Reader pak;
    if (!pak.init_with_data((const uint8_t *)BinaryData::banks_pak, BinaryData::banks_pakSize))
        assert(false);

    PopupMenu pak_submenu;
    uint32_t pak_entries = pak.entry_count();
    if (pak_entries > 0) {
        for (uint32_t i = 0; i < pak_entries; ++i)
            pak_submenu.addItem(2 + i, pak.name(i));
        menu.addSubMenu("Load from collection", pak_submenu);
    }

    int selection = menu.showAt(clicked);
    if (selection == 1) {
        FileChooser chooser(TRANS("Load bank..."), bank_directory_, file_filter, prefer_native_file_dialog);
        if (chooser.browseForFileToOpen()) {
            File file = chooser.getResult();
            bank_directory_ = file.getParentDirectory();
            load_bank(file);
        }
    }
    else if (selection >= 2) {
        uint32_t index = selection - 2;
        const std::string &name = pak.name(index);
        std::string wopl = pak.extract(index);
        self()->load_bank_mem((const uint8_t *)wopl.data(), wopl.size(), name);
    }
}

template <class T>
void Generic_Main_Component<T>::handle_save_bank(Component *clicked)
{
#if defined(ADLPLUG_OPL3)
    const char *file_filter = "*.wopl";
    const char *file_extension = ".wopl";
#elif defined(ADLPLUG_OPN2)
    const char *file_filter = "*.wopn";
    const char *file_extension = ".wopn";
#endif

    FileChooser chooser(TRANS("Save bank..."), bank_directory_, file_filter, prefer_native_file_dialog);
    if (!chooser.browseForFileToSave(false))
        return;

    File file = chooser.getResult();
    file = file.withFileExtension(file_extension);

    bool confirm = true;
    if (file.exists()) {
        String title = TRANS("File already exists");
        String message = TRANS("There's already a file called: ")
            + file.getFullPathName() + "\n\n" +
            TRANS("Are you sure you want to overwrite it?");
        confirm = AlertWindow::showOkCancelBox(
            AlertWindow::WarningIcon, title, message,
            TRANS("Overwrite"), TRANS("Cancel"), this);
    }

    if (confirm) {
        bank_directory_ = file.getParentDirectory();
        self()->save_bank(file);
    }
}

template <class T>
void Generic_Main_Component<T>::load_bank(const File &file)
{
    trace("Load from WOPL file: %s", file.getFullPathName().toRawUTF8());

    std::unique_ptr<uint8_t[]> filedata;
    std::unique_ptr<FileInputStream> stream(file.createInputStream());
    uint64_t length;
    constexpr uint64_t max_length = 8 * 1024 * 1024;
    const char *error_title = "Error loading bank";

    if (stream->failedToOpen()) {
        AlertWindow::showMessageBox(
            AlertWindow::WarningIcon, error_title, "The file could not be opened.");
        return;
    }

    if ((length = stream->getTotalLength()) >= max_length) {
        AlertWindow::showMessageBox(
            AlertWindow::WarningIcon, error_title, "The selected file is too large to be valid.");
        return;
    }

    filedata.reset(new uint8_t[length]);
    if (stream->read(filedata.get(), length) != length) {
        AlertWindow::showMessageBox(
            AlertWindow::WarningIcon, error_title, "The input operation has failed.");
        return;
    }

    self()->load_bank_mem(filedata.get(), length, file.getFileNameWithoutExtension());
}

template <class T>
void Generic_Main_Component<T>::handle_change_keymap()
{
    MidiKeyboardComponent &kb = *self()->midi_kb;
    PopupMenu menu;
    build_key_layout_menu(menu, last_key_layout_);
    int selection = menu.showMenu(PopupMenu::Options()
                                  .withParentComponent(this));
    if (selection != 0)
        last_key_layout_ = set_key_layout(kb, (Key_Layout)(selection - 1), *conf_);
    kb.grabKeyboardFocus();
}

template <class T>
void Generic_Main_Component<T>::handle_change_octave(int diff)
{
    MidiKeyboardComponent &kb = *self()->midi_kb;
    int octave = midi_kb_octave_ + diff;
    octave = (octave < 0) ? 0 : octave;
    octave = (octave > 10) ? 10 : octave;
    if (octave != midi_kb_octave_) {
        midi_kb_octave_ = octave;
        kb.setKeyPressBaseOctave(octave);
    }
    kb.grabKeyboardFocus();
}

template <class T>
void Generic_Main_Component<T>::set_int_parameter_with_delay(unsigned delay, AudioParameterInt &p, int v)
{
    const String &id = p.paramID;
    std::unique_ptr<Timer> &slot = parameters_delayed_[id];

    if (slot)
        trace("Cancel delayed parameter %s", id.toRawUTF8());
    trace("Schedule delayed parameter %s in %u ms", id.toRawUTF8(), delay);

    Timer *timer = Functional_Timer::create1(
                    [&p, v](Timer *t){
                        t->stopTimer();
                        trace("Set delayed parameter %s now", p.paramID.toRawUTF8());
                        p = v;
                    });
    slot.reset(timer);
    timer->startTimer(delay);
}

template <class T>
void Generic_Main_Component<T>::handleNoteOn(MidiKeyboardState *, int channel_, int note, float velocity)
{
    unsigned channel = (unsigned)(channel_ - 1);

    if (is_percussion_channel(channel))
        note = (midiprogram_[channel] - 128) & 127;

    uint8_t midi[3];
    midi[0] = channel | (0b1001u << 4);
    midi[1] = note;
    midi[2] = velocity * 127;
    write_to_processor(User_Message::Midi, midi, 3);
}

template <class T>
void Generic_Main_Component<T>::handleNoteOff(MidiKeyboardState *, int channel_, int note, float velocity)
{
    unsigned channel = (unsigned)(channel_ - 1);

    if (is_percussion_channel(channel))
        note = (midiprogram_[channel] - 128) & 127;

    uint8_t midi[3];
    midi[0] = channel | (0b1000u << 4);
    midi[1] = note;
    midi[2] = velocity * 127;
    write_to_processor(User_Message::Midi, midi, 3);
}

template <class T>
void Generic_Main_Component<T>::focusGained(FocusChangeType cause)
{
    if (self()->midi_kb)
        self()->midi_kb->grabKeyboardFocus();
}

template <class T>
void Generic_Main_Component<T>::globalFocusChanged(Component *component)
{
    ComponentPeer *peer = getPeer();
    Component *window = nullptr;
    if (peer)
        window = &peer->getComponent();

    if (component == window)
        grabKeyboardFocus();
}

template <class T>
bool Generic_Main_Component<T>::write_to_processor(
    User_Message tag, const void *msgbody, unsigned msgsize)
{
    AdlplugAudioProcessor &proc = *proc_;

    Message_Header hdr(tag, msgsize);

    Buffered_Message msg;
    while (!msg) {
        std::shared_ptr<Simple_Fifo> queue = proc.message_queue_for_ui();
        if (!queue)
            return false;
        msg = Messages::write(*queue, hdr);
        if (!msg)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else {
            std::memcpy(msg.data, msgbody, msgsize);
            Messages::finish_write(*queue, msg);
        }
    }

    return true;
}