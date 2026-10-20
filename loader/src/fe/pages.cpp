// The in-game PC SETTINGS pages, built at start from the user's own front-end files (nothing of the game is in the
// port): copies of the game's own pages with the port's rows (fe/pc_settings.cpp, fe/bind_list.cpp).
//
// Main menu (FEMain.bin):
// - optionsmenuxbox (Options): a fifth row PC SETTINGS that opens page PCSettings. Its TextAnim (char 27) places the
//   rows transparent at depths 1/3/5/7 in frame 0; frames 46/49/52/55 make each opaque (Move + colour transform) and
//   drop a flash bar at depths 9-21. The new row: depth 2, its own reveal at frame 58 with a bar at depth 22.
//   PagePrepare / PageRelease / PageDestruct are renamed and wrapped: the wrappers add pcButton (BkMenuButton) to
//   the up/down ring.
// - pcsettings: a copy of Options; the four rows open the groups' pages, Back goes to Options instead of Options' own
//   save-and-leave.
// - a page per group and the KEY BINDINGS page: copies of Sound Setup (TextAnim char 49, six row slots revealed at
//   frames 43-58). A choice row is a clone of the PLAY MODE row (char 25: label 24 > 23, value 22 > 21, arrows 19)
//   whose value text shows the port's &PC_V<row> and whose BkListBox calls PCChange(row, -1 / +1). A slider row, a
//   link row and a key row are clones of the SFX row (char 40 > 39 > text 38): a slider adds the bullet bar (char 34,
//   26 frames) at 45 % on the value line and the number after it (BkMovieclipSlider from externObject.FEPCS<row>,
//   calling PCSlider<row>(0-100)); a key row adds its keys to the right.
// Pause menu (InGameOn.bin, PseMenu.bin): PC SETTINGS after OPTIONS opens one page whose rows the port fills
// (fe/pause_ui.cpp); see "pause menu" below.
// Captions are literal text: the game's text provider (FUN_001184D0) shows a text field's variable string as it is
// unless it starts with '$' (localized), '&' (a value) or '#'.
#include "fe/pages.h"

#include "common.h"
#include "fe/apt.h"
#include "fe/bind_list.h"
#include "fe/pause_ui.h"
#include "fe/pc_settings.h"
#include "fe/vars.h"
#include "input/bindings.h"
#include "lang.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace {

using namespace apt;
using Push = std::function<void(Asm &)>;
using Path = std::vector<std::string>;

Push S(const std::string &s) { return [s](Asm &a) { a.push(Value::S(s)); }; }
Push I(int n) { return [n](Asm &a) { a.push(Value::I(n)); }; }
Push undefined() { return [](Asm &a) { a.push(Value::U()); }; }
Push nothing_fn() { return [](Asm &a) { a.function2("", [](Asm &) {}, 0x29); }; } // function () {}
Push at(const Path &p)
{
    return [p](Asm &a) {
        a.push(Value::S(p[0])).get_var();
        for (size_t i = 1; i < p.size(); i++) a.push(Value::S(p[i])).get_member();
    };
}

void call(Asm &a, const Path &obj, const std::string &method, const std::vector<Push> &args = {})
{
    for (auto it = args.rbegin(); it != args.rend(); ++it) (*it)(a);
    a.push(Value::I((int)args.size()));
    at(obj)(a);
    a.push(Value::S(method)).call_method().pop();
}

void call_function(Asm &a, const std::string &name) { a.push({Value::I(0), Value::S(name)}).call_function().pop(); }

// The same call with the function's value left on the stack (an expression rather than a statement).
Push call_value(const std::string &name)
{
    return [name](Asm &a) { a.push({Value::I(0), Value::S(name)}).call_function(); };
}

void set(Asm &a, const Path &obj, const std::string &member, const Push &value)
{
    at(obj)(a);
    a.push(Value::S(member));
    value(a);
    a.set_member();
}

void make(Asm &a, const std::string &var, const std::string &cls, const std::vector<Push> &args = {})
{
    a.push(Value::S(var));
    for (auto it = args.rbegin(); it != args.rend(); ++it) (*it)(a);
    a.push({Value::I((int)args.size()), Value::S(cls)}).new_object().set_var();
}

// externObject's call fields, as the pages' own list objects fill them (the port's dispatcher takes "PC..." names)
void trigger(Asm &f, const std::string &name, const std::vector<Push> &params)
{
    const char *fields[3] = {"FEFunctionParam1", "FEFunctionParam2", "FEFunctionParam3"};
    set(f, {"_global", "externObject"}, "FEFunctionTrigger", I(1));
    set(f, {"_global", "externObject"}, "FEFunctionName", S(name));
    for (size_t i = 0; i < params.size() && i < 3; i++) set(f, {"_global", "externObject"}, fields[i], params[i]);
}

void literal(Movie &mv, uint32_t text, const std::string &s)
{
    if (rd(mv.apt, text) != 2) throw Error("not a text character");
    uint32_t p = mv.cstring(s);
    wr(mv.apt, text + 52, p);
    wr(mv.apt, text + 56, p);
}

Bytes body(const Movie &mv, uint32_t off) { return Bytes(mv.apt.begin() + off, mv.apt.begin() + off + 64); }

// A page defines its classes (BkMenuList, BkMenuButton and, by page, BkListBox, BkMovieclipSlider,
// BkMultiChoiceMenuButton, BkValueList, BkValuePoint) in DoInitActions only when _global has none yet, makes
// MyMenu = new BkMenuList in Prepare, and its Destruct deletes MyMenu, _global.BkMenuList, _global.BkMenuButton (and
// on some pages more of them). A DoInitAction runs only when the runtime loads the movie: a page entered again soon
// after it was left (still loaded) defined nothing, the page before had deleted the classes, so Prepare made no
// MyMenu (no focus, no Back: a dead page; the debug log showed the load seeing no _global.BkMenuList).
// Nothing of a page may stay in _global after its container goes, and a page must not release what another left
// there: a function keeps a reference to its container's movie entry, the game unloads containers (FEMain.bin at a
// mission start, PseMenu.bin at a resume when it needs the room) whatever is still referenced, and the entry released
// or run later walks freed or unrelocated data (crashes: FUN_000FC360 at the first pause; FUN_0010D8D0, frame table
// 0xA70, after the pause page's PcRow stayed in _global over a PseMenu reload). So on the pages the port changes:
// - first in frame 0, the page keeps what _global held for each of its classes (pcPrev<class>);
// - the class definitions run at every load (Movie::define_classes_on_load), and the page keeps them (pcOwn<class>);
//   the port's own classes (PcRow, KeyList) count as the page's;
// - Destruct runs the page's own Destruct0, then each class goes back to what the page found (pcPrev) where it is
//   still the page's, or stays as another page set it;
// - Update: a page (not destroyed) without MyMenu or its first button (row_var) puts its own classes back, makes MyMenu
//   again with what Prepare sets on it and runs PagePrepare again (a safety net).
using Classes = std::vector<std::string>;
std::string own_var(const std::string &cls) { return "pcOwn" + cls; }
std::string prev_var(const std::string &cls) { return "pcPrev" + cls; }
std::string keep_var(const std::string &cls) { return "pcKeep" + cls; }

bool fe_debug() { return getenv("BLACK_FE_DEBUG") != nullptr; }

Push defined(const Path &p) // true / false
{
    return [p](Asm &a) {
        at(p)(a);
        a.push(Value::U()).equals2().logical_not();
    };
}

// tests (BLACK_FE_DEBUG): queues an event in _global.pcE<n> (a short string: the call fields are 60-byte buffers the
// runtime fills without a bound, a longer string overruns them); dbg_flush sends one per update as PCLog
void dbg_log(Asm &a, const std::vector<Push> &parts)
{
    if (!fe_debug()) return;
    at({"_global"})(a);
    a.push(Value::S("pcE"));
    at({"_global", "pcEn"})(a);
    a.add2();
    a.push(Value::S(""));
    for (const Push &x : parts) {
        x(a);
        a.add2();
    }
    a.set_member();
    set(a, {"_global"}, "pcEn", [](Asm &g) { at({"_global", "pcEn"})(g); g.push(Value::I(1)).add2(); });
}

void dbg_flush(Asm &f)
{
    if (!fe_debug()) return;
    at({"_global", "pcEf"})(f);
    at({"_global", "pcEn"})(f);
    f.equals2();
    size_t empty = f.branch(0x9D);
    at({"_global", "externObject", "FEFunctionTrigger"})(f); // a call is waiting: next update
    f.push(Value::I(1)).equals2();
    size_t busy = f.branch(0x9D);
    trigger(f, "PCLog", {[](Asm &g) {
                at({"_global"})(g);
                g.push(Value::S("pcE"));
                at({"_global", "pcEf"})(g);
                g.add2().get_member();
            }});
    set(f, {"_global"}, "pcEf", [](Asm &g) { at({"_global", "pcEf"})(g); g.push(Value::I(1)).add2(); });
    f.land(empty);
    f.land(busy);
}

// more: the port's own classes the page defines in its script (before own_classes_script)
Classes own_classes_prepare(Movie &mv, const Classes &more = {})
{
    Classes classes = mv.define_classes_on_load();
    if (std::find(classes.begin(), classes.end(), "BkMenuList") == classes.end()) throw Error("a page without BkMenuList");
    classes.insert(classes.end(), more.begin(), more.end());
    mv.rename_function(mv.main_script(), "Destruct", "Destruct0");
    Asm a(mv); // before the definitions: what _global holds now
    for (const std::string &cls : classes) {
        a.push(Value::S(prev_var(cls)));
        at({"_global", cls})(a);
        a.set_var();
    }
    mv.prepend_action(a.finish());
    return classes;
}

void own_classes_script(Asm &a, const Classes &classes, const std::string &tag)
{
    if (fe_debug()) { // pcInst: this page instance's number
        at({"_global", "pcN"})(a);
        a.push(Value::U()).equals2().logical_not();
        size_t has = a.branch(0x9D);
        set(a, {"_global"}, "pcN", I(0));
        a.land(has);
        set(a, {"_global"}, "pcN", [](Asm &g) { at({"_global", "pcN"})(g); g.push(Value::I(1)).add2(); });
        a.push(Value::S("pcInst"));
        at({"_global", "pcN"})(a);
        a.set_var();
        at({"_global", "pcEn"})(a);
        a.push(Value::U()).equals2().logical_not();
        size_t log = a.branch(0x9D);
        set(a, {"_global"}, "pcEn", I(0));
        set(a, {"_global"}, "pcEf", I(0));
        a.land(log);
        dbg_log(a, {S("L" + tag + "#"), at({"pcInst"}), S(" g="), defined({"_global", "BkMenuList"}), S(" n=" + std::to_string(classes.size())),
                    S(" row="), defined({"_global", "PcRow"}), S(" key="), defined({"_global", "KeyList"}), S(" sl="), defined({"_global", "BkMovieclipSlider"})});
    }
    for (const std::string &cls : classes) { // at load: the classes this page just defined
        a.push(Value::S(own_var(cls)));
        at({"_global", cls})(a);
        a.set_var();
    }
    a.function2("Destruct", [classes, tag](Asm &f) {
        dbg_log(f, {S("D" + tag + "#"), at({"pcInst"}), S(" eq="), [](Asm &g) { at({"_global", "BkMenuList"})(g); at({own_var("BkMenuList")})(g); g.equals2(); }});
        f.push(Value::S("pcDead")).push_true().set_var();
        for (const std::string &cls : classes) { // after Destruct0: what the page found, or another page's class
            f.push(Value::S(keep_var(cls)));
            at({"_global", cls})(f);
            at({own_var(cls)})(f);
            f.equals2();
            size_t own = f.branch(0x9D);
            at({"_global", cls})(f);
            size_t done = f.branch(0x99);
            f.land(own);
            at({prev_var(cls)})(f);
            f.land(done);
            f.set_var();
        }
        call_function(f, "Destruct0");
        for (const std::string &cls : classes) {
            at({keep_var(cls)})(f);
            f.push(Value::U()).equals2();
            size_t none = f.branch(0x9D);
            set(f, {"_global"}, cls, at({keep_var(cls)}));
            size_t done = f.branch(0x99);
            f.land(none);
            at({"_global"})(f);
            f.push(Value::S(cls)).op(0x3A).pop(); // Delete
            f.land(done);
            for (const std::string &v : {keep_var(cls), prev_var(cls), own_var(cls)}) f.push(Value::S(v)).delete2().pop();
        }
    });
}

// The heal itself: the page's own classes back, the menu again where it is gone, and the page's own Prepare.
void emit_heal_body(Asm &f, const Classes &classes, const std::string &tag)
{
    auto gone = [&f](const Path &p) {
        at(p)(f);
        f.push(Value::U()).equals2();
    };
    dbg_log(f, {S("H" + tag + "#"), at({"pcInst"}), S(" own="), defined({own_var("BkMenuList")}), S(" g="), defined({"_global", "BkMenuList"}),
                S(" menu="), defined({"MyMenu"})});
    for (const std::string &cls : classes) { // its own classes back
        gone({own_var(cls)});
        size_t none = f.branch(0x9D);
        set(f, {"_global"}, cls, at({own_var(cls)}));
        f.land(none);
    }
    gone({"_global", "BkMenuList"}); // nothing to make it from: leave it
    size_t fine2 = f.branch(0x9D);
    gone({"MyMenu"});
    f.logical_not();
    size_t has_menu = f.branch(0x9D);
    f.push(Value::S("MyMenu"));
    f.push({Value::I(0), Value::S("BkMenuList")}).new_object().set_var();
    const char *prompts[4][2] = {{"SetButtonSelector", "ButtonSelector"}, {"SetSelectPrompt", "SelectPrompt"},
                                 {"SetConfirmPrompt", "ConfirmPrompt"}, {"SetBackPrompt", "BackPrompt"}};
    for (auto &pr : prompts) call(f, {"MyMenu"}, pr[0], {at({"Main", pr[1]})});
    f.land(has_menu);
    call_function(f, "PagePrepare");
    trigger(f, "PCHealed", {});
    f.land(fine2);
}

void emit_heal(Asm &f, const std::string &row_var, const Classes &classes, const std::string &tag)
{
    auto gone = [&f](const Path &p) {
        at(p)(f);
        f.push(Value::U()).equals2();
    };
    at({"pcDead"})(f);
    size_t fine0 = f.branch(0x9D);
    gone({"MyMenu"});
    size_t heal = f.branch(0x9D);
    gone({row_var});
    f.logical_not();
    size_t fine1 = f.branch(0x9D);
    f.land(heal);
    emit_heal_body(f, classes, tag);
    f.land(fine0);
    f.land(fine1);
}

// A page of the game's own: the port has no row of its own to test and its Prepare is not the port's to run again (the
// game's own pages build their rows once), so a page of the game's own is healed by the class definitions alone.

// The pointer in the pages the port builds: defined below (the classes and the tools the pages use), called by the
// Update of a page whose rows should follow the cursor. ids: the setting each row is (-1: none), so the port can
// change a value row itself (the game's widgets only take a direction that is held, which a pointer cannot make).
void hover_block(Asm &f, const std::vector<std::string> &rows, const std::vector<int> &ids);

void heal_update(Asm &a, const std::string &row_var, const Classes &classes, const std::string &tag,
                 const std::vector<std::string> &hover = {})
{
    a.function2("Update", [row_var, classes, tag, hover](Asm &f) {
        emit_heal(f, row_var, classes, tag);
        call_function(f, "Update0");
        hover_block(f, hover, {});
        dbg_flush(f);
    });
}

std::string lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string ingame(const std::string &page) { return page + "IG"; }

// ---------------------------------------------------------------------------------------------------- rows
struct RowSpec {
    enum Kind { Choice, Slider, Link, Key } kind;
    int id;            // the setting (Choice, Slider) or the list row (Key)
    std::string label; // caption text: literal, or a variable for Key rows
    std::string value; // the value's variable ("" for Link)
    std::string page;  // Link: the page it opens
};

std::vector<RowSpec> group_rows(const pcset::Group &g, bool pause)
{
    std::vector<RowSpec> out;
    for (int n : g.rows) {
        if (pause && pcset::restart_only(n) && !pcset::is_frame_rate(n)) continue; // applies at the next start: main menu only
        std::string value = "&PC_V" + std::to_string(fevar::kValue + (int)out.size());
        if (const char *page = pcset::link(n)) out.push_back({RowSpec::Link, n, pcset::label(n), "", pause ? ingame(page) : page});
        else out.push_back({pcset::is_slider(n) ? RowSpec::Slider : RowSpec::Choice, n, pcset::label(n), value, ""});
    }
    return out;
}

std::vector<RowSpec> key_rows()
{
    std::vector<RowSpec> out;
    for (int i = 0; i < bindlist::kRows; i++)
        out.push_back({RowSpec::Key, i, "&PC_V" + std::to_string(fevar::kLabel + i), "&PC_V" + std::to_string(fevar::kValue + i), ""});
    return out;
}

// _global.<name>: a list object for rows, prototype.ProcessMapInput(code) and an empty Release. The menu calls
// ProcessMapInput on every update for the row that has the focus (FEInputId 0: no input).
void define_list_class(Asm &a, const std::string &name, const std::function<void(Asm &)> &input)
{
    at({"_global"})(a);
    a.push(Value::S(name));
    a.function2("", [](Asm &) {}, 0x29);
    a.set_member();
    for (const char *m : {"ProcessMapInput", "Release"}) {
        at({"_global", name, "prototype"})(a);
        a.push(Value::S(m));
        std::function<void(Asm &)> code = m[0] == 'P' ? input : std::function<void(Asm &)>([](Asm &) {});
        a.function2("", code, 0x129); // register 1 = this, 2 = _global
        a.set_member();
    }
}

// The packing the port's own rows report (hover_block / fe/calls.cpp pc_hover): kPortBase + (id + 1) * kPackId +
// middle * kPackW + width. kPortBase keeps it clear of the game's own rows (hover_walk: the middle itself); kPackW
// holds the middle and the width (both under two thousand stage pixels), and kPackId a whole (middle, width) pair.
constexpr int kPortBase = 0x10000000, kPackW = 2048, kPackId = kPackW * kPackW;

void input_id(Asm &f) { f.push(Value::R(2)).push(Value::S("externObject")).get_member().push(Value::S("FEInputId")).get_member(); }

// The pointer in the menus the port builds. The game's own menus are the pad's only, so input/bindings.cpp feeds each
// of the port's pages the pointer's point through externObject.FEPointerX / FEPointerY (the picture's pixels 0-640 by
// 0-480; both -1 while the pointer is not the input used last) and the page puts the focus on the row the pointer is
// on, so the highlight follows the cursor. The pages read the port's own copies of those two (FEPCPointerX / Y): the
// game's own front-end code writes over FEPointerX/Y in the middle of its update (it puts -1 there when its own
// pointer is not the input in use), so a page reading them could see the cursor gone where the port had just put it --
// and the row under the cursor was then unknown and a click on it did nothing, now and then, by timing alone. A row's
// line is its _y plus every ancestor's up to the root (the pages neither rotate nor scale), which is the page's own
// pixels: the pause page's sliding panel and every page's own layout need nothing from the port. The row the pointer
// is on is the last one whose line is above it (a row owns the band down to the next row's line, so no row is out of
// reach), nothing above the first row. A row the page hid (the pause page shows a few at a time) holds nothing. The
// focus is set once per change, so the game's focus animation and sound play once, and the row is reported as
// PCHover(row) for the tests.
void hover_block(Asm &f, const std::vector<std::string> &rows, const std::vector<int> &ids)
{
    if (rows.empty()) return;
    auto var = [](const char *name) { return [name](Asm &g) { at({name})(g); }; };
    auto let = [](Asm &g, const char *name, const Push &value) {
        g.push(Value::S(name));
        value(g);
        g.set_var();
    };
    auto pointer = [](const char *name) {
        return [name](Asm &g) { at({"_global", "externObject", name})(g); };
    };
    auto clip = [&rows](Asm &g, size_t i) {
        at({rows[i]})(g);
        g.push(Value::S("movieClip")).get_member();
    };
    // the row's place in the page's own pixels: its <member> plus each ancestor's, up to the root (_y for the row's
    // line, _x for the pointer's side of it)
    auto axis_of = [&](size_t i, const std::string &member) {
        return [&, i, member](Asm &g) {
            let(g, "pcWalk", [&](Asm &h) { clip(h, i); });
            let(g, "pcSum", I(0));
            for (int level = 0; level < 8; level++) {
                var("pcWalk")(g);
                g.push(Value::U()).equals2(); // the cursor ran off the top: the sum is done
                size_t end1 = g.branch(0x9D);
                var("pcWalk")(g);
                at({"_root"})(g);
                g.equals2(); // the root's own place is the page's origin
                size_t end2 = g.branch(0x9D);
                g.push(Value::S("pcSum"));
                var("pcSum")(g);
                var("pcWalk")(g);
                g.push(Value::S(member)).get_member();
                g.add2();
                g.set_var();
                g.push(Value::S("pcWalk"));
                var("pcWalk")(g);
                g.push(Value::S("_parent")).get_member();
                g.set_var();
                g.land(end2);
                g.land(end1);
            }
            var("pcSum")(g);
        };
    };
    auto line_of = [&](size_t i) { return axis_of(i, "_y"); };
    // The port bumps externObject.FEPCPage at every page change (input/bindings.cpp). A page the player comes back to
    // (back, then the row again) has the same row under the cursor, and this page reports a row only when it changes:
    // without the bump it would answer "the same row as last time" and never say a word, while the port, which drops
    // the row it kept at every page change, waits for a report that never comes -- the click lands on no row at all.
    pointer("FEPCPage")(f);
    var("pcGen")(f);
    f.equals2();
    f.logical_not();
    size_t new_visit = f.branch(0x9D);
    let(f, "pcGen", pointer("FEPCPage"));
    let(f, "pcHover", I(-1)); // the row reported on the visit that just went: report the one under the cursor again
    f.land(new_visit);
    let(f, "pcLine", I(-9999)); // the lowest row line above the pointer the search has seen
    let(f, "pcHalf", I(0)); // half the gap between two rows: the bands sit on the lines, not under them
    let(f, "pcOver", I(-1));
    if (rows.size() > 1) {
        f.push(Value::S("pcHalf"));
        line_of(1)(f);
        line_of(0)(f);
        f.op(0x0B); // the gap between the first two rows
        f.push(Value::I(2)).op(0x0D);
        f.set_var();
    }
    for (size_t i = 0; i < rows.size(); i++) {
        defined({rows[i]})(f); // a button this page does not carry (two pages may share one list): skip it
        f.logical_not();
        size_t missing = f.branch(0x9D);
        clip(f, i);
        f.push(Value::S("_visible")).get_member();
        f.push(Value::I(0)).equals2(); // only a row the page itself hid: a missing value counts as shown
        size_t hidden = f.branch(0x9D);
        f.push(Value::S("pcRow"));
        line_of(i)(f);
        f.set_var();
        var("pcRow")(f);
        pointer("FEPCPointerY")(f);
        var("pcHalf")(f);
        f.add2(); // the pointer's line, the band's bottom on it
        f.op(0x67); // below the pointer's line: not the row it is on
        size_t below = f.branch(0x9D);
        pointer("FEPCPointerY")(f);
        var("pcRow")(f);
        var("pcHalf")(f);
        f.add2();
        f.op(0x67); // more than half a gap below the row's line: not the row it is on either
        size_t belowband = f.branch(0x9D);
        var("pcRow")(f);
        var("pcLine")(f);
        f.op(0x67); // an earlier row already owns this band: the best line stays
        f.logical_not();
        size_t lower = f.branch(0x9D);
        f.push(Value::S("pcLine"));
        var("pcRow")(f);
        f.set_var();
        f.push(Value::S("pcOver"));
        f.push(Value::I((int)i));
        f.set_var();
        f.land(lower);
        f.land(below);
        f.land(belowband);
        f.land(hidden);
        f.land(missing);
    }
    var("pcOver")(f); // the row it put the focus on last: nothing to do while it is the same one
    var("pcHover")(f);
    f.equals2();
    size_t same = f.branch(0x9D);
    let(f, "pcHover", var("pcOver")); // -1 (the pointer is elsewhere) matches no row: the focus stays where it was
    var("pcOver")(f);
    f.push(Value::I(-1)).op(0x67).logical_not(); // pcOver <= -1: the pointer is on no row
    size_t norow = f.branch(0x9D);
    for (size_t i = 0; i < rows.size(); i++) {
        var("pcOver")(f);
        f.push(Value::I((int)i)).equals2().logical_not();
        size_t other = f.branch(0x9D);
        call(f, {"MyMenu"}, "SetFocusButton", {at({rows[i]})});
        at({"_global", "externObject", "FEFunctionTrigger"})(f); // a call is waiting: next update
        f.push(Value::I(1)).equals2();
        size_t busy = f.branch(0x9D);
        // The row, whether it carries a value list, and the setting it is with the row's middle as the packed third
        // parameter (fe/calls.cpp pc_hover): kPortBase + (id + 1) * kPackId + the middle * kPackW + the width. The
        // port asks a click's side against the middle when the button goes down (input/bindings.cpp menu_row), and a
        // slider takes the pointer's place along the row, which needs the row's width as well. kPortBase tells this
        // apart from a row of the game's own (fe/pages.cpp hover_walk), whose code is the middle itself (id -1).
        let(f, "pcCode", I(kPortBase + ((i < ids.size() ? ids[i] : -1) + 1) * kPackId));
        f.push(Value::S("pcCode"));
        var("pcCode")(f);
        axis_of(i, "_x")(f); // the row's left
        clip(f, i);
        f.push(Value::S("_width")).get_member();
        f.push(Value::I(2)).op(0x0D);
        f.add2(); // the row's left plus half its width: its middle
        f.push(Value::I(kPackW)).op(0x0C); // the middle, in the id's slot
        clip(f, i);
        f.push(Value::S("_width")).get_member();
        f.add2(); // the row's width, below the middle
        f.add2(); // the setting and the middle, as pc_hover unpacks them
        f.set_var();
        trigger(f, "PCHover", {I((int)i), defined({rows[i], "mChoiceList"}), var("pcCode")});
        f.land(busy);
        f.land(other);
    }
    size_t done = f.branch(0x99); // the pointer is on a row: it reported itself above
    f.land(norow);
    trigger(f, "PCHover", {I(-1), I(0), I(0)}); // off the rows (no row, no value, no middle): the port drops its row
    f.land(done);
    f.land(same);
}

// The walk's two helpers, defined once per page (game_page) rather than expanded at every step. Both keep their state
// and take their arguments on _global (see hover_walk): the place of a clip, and one step of the ring. Expanded inline
// at each of the walk's 64 steps the place alone was about two thirds of a page's ActionScript -- 260 KB a page, which
// the game could not allocate for the rebuilt front end (the disc error screen), against 8 KB for both as functions.
//
// The place of a clip on the page in pixels: its <member> plus each ancestor's, up to the root. _global.pcArgClip is
// the clip, _global.pcArgMem the member (_y for a row's line, _x for its middle); the sum is returned.
void pc_place(Asm &f)
{
    auto var = [](const char *name) { return [name](Asm &g) { at({"_global", name})(g); }; };
    auto let = [](Asm &g, const char *name, const Push &value) { set(g, {"_global"}, name, value); };
    let(f, "pcWalk", var("pcArgClip"));
    let(f, "pcSum", I(0));
    for (int level = 0; level < 8; level++) {
        var("pcWalk")(f);
        f.push(Value::U()).equals2(); // the walk ran off the top: the sum is done
        size_t end1 = f.branch(0x9D);
        var("pcWalk")(f);
        at({"_root"})(f);
        f.equals2(); // the root's own place is the page's origin
        size_t end2 = f.branch(0x9D);
        let(f, "pcSum", [&](Asm &g) {
            var("pcSum")(g);
            var("pcWalk")(g);
            var("pcArgMem")(g);
            g.get_member();
            g.add2();
        });
        let(f, "pcWalk", [&](Asm &g) { var("pcWalk")(g); g.push(Value::S("_parent")).get_member(); });
        f.land(end2);
        f.land(end1);
    }
    var("pcSum")(f);
    f.ret();
}

// One button of the ring: its clip's line, the gap between the first two rows (the band on a row's line, kept once),
// and whether it is the row nearest the cursor so far. The link to follow round the ring is _global.pcLink
// (buttonDown or buttonUp) and the button walked _global.pcW. When the ring runs out, closes on the first button, or
// the button has no clip to give a line, this sets _global.pcStop: the caller's steps do nothing further.
void pc_step(Asm &f, bool grid)
{
    auto var = [](const char *name) { return [name](Asm &g) { at({"_global", name})(g); }; };
    auto let = [](Asm &g, const char *name, const Push &value) { set(g, {"_global"}, name, value); };
    auto ptr = [](const char *name) {
        return [name](Asm &g) { at({"_global", "externObject", name})(g); };
    };
    let(f, "pcC", [&](Asm &g) { var("pcW")(g); g.push(Value::S("movieClip")).get_member(); });
    var("pcC")(f);
    f.push(Value::U()).equals2(); // a button without a clip has no place to give
    size_t no_clip = f.branch(0x9D);
    let(f, "pcArgClip", var("pcC"));
    let(f, "pcArgMem", S("_y"));
    let(f, "pcRaw", call_value("pcPlace")); // the clip's registration point: the top of a row of text or of a key
    // half the gap between the first two rows the walk saw (measured on the tops: the same gap as on the middles)
    var("pcWN")(f);
    f.push(Value::I(0)).op(0x67); // a line is already there
    f.logical_not();
    size_t no_prev = f.branch(0x9D);
    var("pcHalf")(f);
    f.push(Value::I(0)).equals2(); // unset: this pair gives the spacing
    f.logical_not();
    size_t half_set = f.branch(0x9D);
    let(f, "pcGap", [&](Asm &g) { var("pcRaw")(g); var("pcPrev")(g); g.op(0x0B); });
    var("pcGap")(f);
    f.push(Value::I(0)).op(0x67);
    size_t pos = f.branch(0x9D);
    let(f, "pcGap", [&](Asm &g) { var("pcPrev")(g); var("pcRaw")(g); g.op(0x0B); });
    f.land(pos);
    let(f, "pcHalf", [&](Asm &g) { var("pcGap")(g); g.push(Value::I(2)).op(0x0D); });
    f.land(half_set);
    f.land(no_prev);
    // The row's line: the clip's own middle. A row of text registers at its top, so the bare top put the band on the
    // upper half of the row and the pointer on the lower half found no row at all (the picture menu's BRILHO bar, whose
    // slider hangs below its label). It is the clip's own height and not the row spacing on purpose: a spacing measured
    // from the walk's first two buttons changes when the focus (and so the walk's start) changes, which shifted every
    // line by a few units and made two rows at nearly the same distance flip the highlight from update to update.
    let(f, "pcLine", [&](Asm &g) {
        var("pcRaw")(g);
        var("pcC")(g);
        g.push(Value::S("_height")).get_member();
        g.push(Value::I(2)).op(0x0D);
        g.add2();
    });
    // The row nearest the cursor so far. On a one-column page: the distance between the row's line and the pointer's,
    // squared. On a grid, the same with the middle's distance as the tie-break (scaled by 2^20, far more than the whole
    // stage's width squared, so a line difference always outweighs it): every key of a line would otherwise tie, the
    // first one the ring walked would win, and the cursor only ever reached that column (1, A, K, U on the keyboard).
    let(f, "pcD", [&](Asm &g) { var("pcLine")(g); ptr("FEPCPointerY")(g); g.op(0x0B); });
    let(f, "pcD2", [&](Asm &g) { var("pcD")(g); var("pcD")(g); g.op(0x0C); });
    if (grid) {
        let(f, "pcArgClip", var("pcC"));
        let(f, "pcArgMem", S("_x"));
        let(f, "pcMid", [&](Asm &g) {
            call_value("pcPlace")(g);
            var("pcC")(g);
            g.push(Value::S("_width")).get_member();
            g.push(Value::I(2)).op(0x0D);
            g.add2(); // the button's left plus half its width: its middle
        });
        let(f, "pcDX", [&](Asm &g) { var("pcMid")(g); ptr("FEPCPointerX")(g); g.op(0x0B); });
        let(f, "pcKey", [&](Asm &g) {
            var("pcD2")(g);
            g.push(Value::I(1048576)).op(0x0C);
            var("pcDX")(g);
            var("pcDX")(g);
            g.op(0x0C);
            g.add2();
        });
    } else {
        let(f, "pcKey", var("pcD2"));
    }
    var("pcBest")(f);
    f.push(Value::U()).equals2(); // nothing kept yet: this row
    size_t take = f.branch(0x9D);
    var("pcBestKey")(f);
    var("pcKey")(f);
    f.op(0x67); // the row kept is further: this row
    size_t closer = f.branch(0x9D);
    size_t keep = f.branch(0x99); // the row kept stays
    f.land(take);
    f.land(closer);
    let(f, "pcBest", var("pcW"));
    let(f, "pcBestKey", var("pcKey"));
    let(f, "pcBestD2", var("pcD2"));
    let(f, "pcBestOwner", var("pcOwner"));
    f.land(keep);
    let(f, "pcPrev", var("pcRaw"));
    let(f, "pcWN", [&](Asm &g) { var("pcWN")(g); g.push(Value::I(1)).add2(); });
    f.land(no_clip);
    // the next button of the ring, and the two ways the walk ends
    let(f, "pcNext", [&](Asm &g) { var("pcW")(g); var("pcLink")(g); g.get_member(); });
    var("pcNext")(f);
    f.push(Value::U()).equals2();
    size_t out = f.branch(0x9D);
    var("pcNext")(f);
    var("pcStart")(f);
    f.equals2(); // back at the button the walk began with: every row seen
    size_t closed = f.branch(0x9D);
    let(f, "pcDone", I(1));
    let(f, "pcW", var("pcNext"));
    size_t walked = f.branch(0x99);
    f.land(out);
    f.land(closed);
    let(f, "pcStop", I(1));
    f.land(walked);
}

// The pointer on a page of the game's own (game_page below). Such a page carries no list of the port's own rows: the
// game's menu object (MyMenu; Main.ButtonSelector is the small set a page's overlay has, walked only when MyMenu found
// no row) keeps its buttons in a ring,
// the focused one at its mFocusButton, each button's next at its buttonDown and its previous at its buttonUp link. The
// walk starts at the focused button and follows the ring both ways, keeping the button whose line on the page is
// nearest the cursor and no further than half a row (off the rows the focus stays where it was). So the highlight
// follows the cursor without the port knowing a single name of the page's own rows, and the ring's own order (the
// game's top to bottom one) gives the row spacing. The row is reported as any page of the port's reports its rows
// (fe/calls.cpp pc_hover) with the setting -1: a click on one of the game's own value rows is one step of the game's
// own widget (input 3 or 4) instead of a select, which would confirm the row and leave the page.
void hover_walk(Asm &f, bool grid)
{
    // The walk's own state lives on _global: a name only SetVariable'd in a page of the game's own (no DefineLocal2
    // from the port's own page script) is not what a later GetVariable finds -- the page's Update runs with a scope
    // chain that does not carry it (the debug log read best=true, w=false, h=undefined for the same update). Members
    // of _global are the port's proven way to keep values across updates (pcEn/pcEf above).
    auto var = [](const char *name) { return [name](Asm &g) { at({"_global", name})(g); }; };
    auto let = [](Asm &g, const char *name, const Push &value) { set(g, {"_global"}, name, value); };
    auto ptr = [](const char *name) {
        return [name](Asm &g) { at({"_global", "externObject", name})(g); };
    };
    auto undef = [](Asm &g) { g.push(Value::U()); };
    let(f, "pcWN", I(0)); // rows the walk saw, then half the gap between the first two (the band on a row's line)
    let(f, "pcPrev", I(0));
    let(f, "pcHalf", I(0));
    let(f, "pcDone", I(0)); // the ring came back to the first button: the other way round has nothing new
    let(f, "pcStop", I(0)); // a step found the ring ended: that direction's remaining steps do nothing
    let(f, "pcBest", undef); // the button nearest the cursor, its distance key and the menu it belongs to
    let(f, "pcBestD2", I(0));
    let(f, "pcBestKey", I(0));
    let(f, "pcBestOwner", undef);
    let(f, "pcCode", I(-2));
    let(f, "pcArgMem", S("_y")); // pc_place's member: the rows' lines are what this walk is after
    const Path menus[2] = {{"MyMenu"}, {"Main", "ButtonSelector"}};
    // The walk's phases, each a chain of pc_step: down / up walk a column (the menu's own top-to-bottom order gives
    // the spacing). On a grid (the profile keyboard) two more phases walk a row with buttonLeft / buttonRight -- its
    // ring links a column with buttonDown/buttonUp and a row with buttonLeft/buttonRight, so without them only the
    // focused column was ever reachable. The last pair starts at the end of the row the horizontal pass found: the
    // keyboard's mode buttons (FEITO, ACENTOS, ...) hang off that column and not off the rows, so after a letter had
    // the focus they were not reachable and a click on FEITO typed a letter. Every other page keeps the two phases it
    // has always had: more phases there reach rows the page's own ring does not order top to bottom, and its highlight
    // could flip between two of them from one update to the next.
    const char *links[6] = {"buttonDown", "buttonUp", "buttonLeft", "buttonRight", "buttonDown", "buttonUp"};    for (size_t mi = 0; mi < 2; mi++) {
        const Path &menu = menus[mi];
        // The page's own list (MyMenu) is the page's menu; the small set an overlay carries (Main.ButtonSelector) is
        // walked only when that found no row at all. Taking the nearest of both at once had the walk flicker between
        // a row of the page and one of the overlay from one update to the next, so a click on a value row carried the
        // direction one update and not the next -- and with no direction the click selects the row and leaves the page.
        size_t have_row = 0;
        if (mi) {
            var("pcBest")(f);
            f.push(Value::U()).equals2();
            f.logical_not();
            have_row = f.branch(0x9D);
        }
        at(menu)(f);
        f.push(Value::U()).equals2();
        size_t no_menu = f.branch(0x9D);
        let(f, "pcOwner", at(menu));
        let(f, "pcW", [&](Asm &g) { at(menu)(g); g.push(Value::S("mFocusButton")).get_member(); });
        var("pcW")(f);
        f.push(Value::U()).equals2();
        size_t no_focus = f.branch(0x9D);
        let(f, "pcFocus", var("pcW")); // every direction starts at the focused button, not where the last one stopped
        for (int dir = 0; dir < (grid ? 6 : 2); dir++) {
            if (dir == 2) {
                // The two horizontal chains start where the vertical ones ended: the row the pointer is on. From a key
                // of that row buttonLeft / buttonRight reach the whole row; started from the focused button (a key of
                // some other column) they would only reach that column, the row under the cursor would come out as
                // the crossing cell and the highlight would flip between two cells from one update to the next.
                var("pcBest")(f);
                f.push(Value::U()).equals2();
                size_t no_best = f.branch(0x9D);
                let(f, "pcFocus", var("pcBest"));
                f.land(no_best);
            }
            if (dir == 3) let(f, "pcLeft", var("pcW")); // the left end of that row, before walking it to the right
            if (dir == 4) {
                let(f, "pcRight", var("pcW")); // the right end, where the horizontal walk to the right ran out
                let(f, "pcFocus", var("pcLeft"));
            }
            // each direction is a walk of its own: its own end, its own start, and its own rows for the spacing the
            // band on a row's line is taken from (pc_step), so a direction that finds a single row cannot pair it with
            // the last row of the direction before
            let(f, "pcStop", I(0));
            let(f, "pcDone", I(0));
            let(f, "pcWN", I(0));
            let(f, "pcPrev", I(0));
            let(f, "pcW", var("pcFocus"));
            let(f, "pcStart", var("pcW"));
            let(f, "pcLink", S(links[dir])); // the link pc_step follows round the ring this way
            for (int step = 0; step < 16; step++) {
                var("pcStop")(f);
                f.push(Value::I(0)).equals2();
                f.logical_not();
                size_t out = f.branch(0x9D); // the ring ran out or closed: the rest of the steps do nothing
                call_function(f, "pcStep");
                f.land(out);
            }
        }
        f.land(no_focus);
        f.land(no_menu);
        if (mi) f.land(have_row);
    }
    // half a row either side of the cursor's line (a page whose ring had a single row keeps a sane band)
    var("pcHalf")(f);
    f.push(Value::I(0)).equals2();
    f.logical_not();
    size_t has_half = f.branch(0x9D);
    set(f, {"_global"}, "pcHalf", I(12));
    f.land(has_half);
    // off the rows (the nearest one is further than half a row away): the pointer is on no row
    var("pcBest")(f);
    f.push(Value::U()).equals2();
    size_t none = f.branch(0x9D);
    var("pcBestD2")(f);
    var("pcHalf")(f);
    var("pcHalf")(f);
    f.op(0x0C);
    f.op(0x67); // the row kept is further than the band
    f.logical_not();
    size_t inband = f.branch(0x9D);
    set(f, {"_global"}, "pcBest", undefined());
    f.land(inband);
    f.land(none);
    // A new visit to the page. The port bumps externObject.FEPCPage at every page change, and the row the port was
    // told about belonged to the page that just went. The walk's own state lives on _global, which outlives the page,
    // so without this the same row under the cursor on the way back (back, then the row again) would never be
    // reported: the port drops the row it kept at the page change and waits for a report that never comes, and every
    // click on that row lands on no row at all.
    ptr("FEPCPage")(f);
    var("pcGen")(f);
    f.equals2();
    f.logical_not();
    size_t new_page = f.branch(0x9D);
    set(f, {"_global"}, "pcGen", ptr("FEPCPage"));
    set(f, {"_global"}, "pcLast", S(""));
    f.land(new_page);
    // The row under the cursor. The game's own highlight takes the focus when the row changes (so its reveal and its
    // sound play once) and the port hears about the row then too: the row (or none), whether it carries a value and
    // the row's middle packed as (id + 1) * 4096 + middle, which the port asks a click's side against when the button
    // goes down (input/bindings.cpp menu_row). A row of the game's own has no setting of the port's (id -1), so its
    // code is the middle itself.
    let(f, "pcName", S(""));
    var("pcBest")(f);
    f.push(Value::U()).equals2();
    size_t no_row = f.branch(0x9D);
    let(f, "pcC", [&](Asm &g) { var("pcBest")(g); g.push(Value::S("movieClip")).get_member(); });
    let(f, "pcName", [&](Asm &g) { var("pcC")(g); g.push(Value::S("_name")).get_member(); });
    f.land(no_row);
    var("pcName")(f);
    var("pcLast")(f);
    f.equals2();
    size_t same = f.branch(0x9D); // the same row (or still no row): the highlight and the port's row stay as they are
    set(f, {"_global"}, "pcLast", var("pcName"));
    var("pcBest")(f);
    f.push(Value::U()).equals2();
    size_t no_focus = f.branch(0x9D);
    var("pcBest")(f);      // the button the menu is to focus: SetFocusButton wants the menu button object itself
    f.push(Value::I(1));   // (BkMenuButton, the one that carries buttonDown/buttonUp/mChoiceList), not its clip --
    var("pcBestOwner")(f); // with the clip the game's own ring of links came apart and the walk picked a different
    f.push(Value::S("SetFocusButton")).call_method().pop(); // row every other update
    let(f, "pcArgClip", var("pcC")); // pc_place wants the clip and the member: the row's left
    let(f, "pcArgMem", S("_x"));
    let(f, "pcCode", [&](Asm &g) {
        call_value("pcPlace")(g);
        var("pcC")(g);
        g.push(Value::S("_width")).get_member();
        g.push(Value::I(2)).op(0x0D);
        g.add2(); // the row's left plus half its width: its middle
    });
    trigger(f, "PCHover", {I(0),
                           [&](Asm &g) {
                               defined({"pcBest", "mChoiceList"})(g);
                               defined({"pcC", "mChoiceList"})(g);
                               g.op(0x11); // or
                           },
                           var("pcCode")});
    size_t reported = f.branch(0x99);
    f.land(no_focus);
    trigger(f, "PCHover", {I(-1), I(0), I(0)}); // off the rows: the port drops the row it kept for a click
    f.land(reported);
    f.land(same);
    // tests: what the walk saw this update (a page of the game's own has no list of names to check it against)
    dbg_log(f, {S("#"), var("pcName"), S("|last="), var("pcLast"), S("|rows="), var("pcWN"), S("|half="),
                var("pcHalf"), S("|d2="), var("pcBestD2"), S("|line="), var("pcLine"), S("|w="),
                defined({"pcW"}), S("|wn="), at({"pcW", "_name"}), S("|dn="), defined({"pcW", "buttonDown"}),
                S("|un="), defined({"pcW", "buttonUp"}), S("|cn="), at({"pcC", "_name"}), S("|t="),
                ptr("FEPCPointerY"), S("|x="), ptr("FEPCPointerX")});
    dbg_log(f, {S("|mclB="), defined({"pcBest", "mChoiceList"}), S("|mclC="), defined({"pcC", "mChoiceList"}),
                S("|bn="), at({"pcBest", "_name"}), S("|fy="), ptr("FEPointerY")});
    // Nothing of a page may stay in _global once the page (or its container) goes -- the rule the port's own pages
    // follow, and the one the walk broke: its state has to live on _global (the page's own scope does not carry it from
    // one update to the next), and that state holds the page's own button, menu and clip objects. Left there they kept
    // the objects alive after FEMain.bin was unloaded at a mission start, and the front end crashed on the next load
    // (`00019f97`, in the refcounted object's destroy, right when the title menu came back after QUIT in the pause
    // menu: the user's "quando eu ... saio da missão para o menu o jogo fecha"). Only numbers and strings survive the
    // update now (the last reported row's name, the page number and the walk's tests); every object reference goes.
    for (const char *name : {"pcBest", "pcOwner", "pcBestOwner", "pcW", "pcC", "pcArgClip", "pcWalk", "pcNext",
                             "pcStart", "pcFocus", "pcLeft", "pcRight"})
        set(f, {"_global"}, name, undefined());
}

// A page of the game's own: no row of the port's is added, so the page's own Prepare, Release and Destruct are left
// alone; only its Update gains the walk above, which gives the row under the cursor the focus.
void game_page(Movie &mv, const std::string &name)
{
    // The profile keyboard is the one page of a grid: keys in rows, with the ring linking a row's keys left to right and
    // a column top to bottom. It gets the wider walk (hover_walk's last four phases and the middle's tie-break).
    const bool grid = name == "virtualkeyboard";
    // The page keeps its classes and its Destruct puts them back, exactly as on the pages the port builds: the page's
    // own Destruct deletes _global.BkMenuList, and the page after it (or this one entered again) would then have no
    // classes to make its menu from. Before the assembler opens: this edits the movie.
    Classes classes;
    bool own = false;
    try {
        classes = own_classes_prepare(mv);
        own = true;
    } catch (const Error &e) {
        logf("fe: %s keeps its own classes: %s", name.c_str(), e.what());
    }
    uint32_t script = mv.main_script();
    mv.rename_function(script, "Update", "Update0");
    Asm a(mv);
    if (own) own_classes_script(a, classes, "gp");
    set(a, {"_global"}, "pcLast", S("")); // the row the walk put the focus on last (hover_walk reads it)
    if (fe_debug()) { // the walk's log needs the port's queue (own_classes_script sets these for its own pages)
        set(a, {"_global"}, "pcEn", I(0));
        set(a, {"_global"}, "pcEf", I(0));
    }
    // The walk's helpers, defined once a page: expanded at every step the place alone was two thirds of a page's
    // ActionScript (260 KB a page, which the game could not allocate for the rebuilt front end).
    a.function2("pcPlace", pc_place);
    a.function2("pcStep", [grid](Asm &f) { pc_step(f, grid); });
    a.function2("Update", [grid](Asm &f) {
        call_function(f, "Update0");
        hover_walk(f, grid);
        dbg_flush(f);
    });
    mv.append_control(mv.main, 0, mv.do_action(a.finish()));
}

// The game's own pages the walk runs on, of one container: every name that is there is patched in place (a movie
// without an Update, or with one the port already changed, is left alone).
bool game_pages(Container &c, std::initializer_list<const char *> names)
{
    bool any = false;
    if (fe_debug()) { // tests: which movies the front end really has (BLACK_FE_DEBUG)
        std::string all;
        for (const std::string &n : c.order) all += n + " ";
        logf("fe: movies: %s", all.c_str());
    }
    for (const char *name : names) {
        auto it = c.movies.find(name);
        if (it == c.movies.end()) {
            logf("fe: the front end has no movie %s", name);
            continue;
        }
        try {
            Movie mv(it->second);
            game_page(mv, name);
            it->second = mv.build();
            any = true;
            logf("fe: the pointer is on the page %s", name);
        } catch (const Error &e) {
            logf("fe: %s left alone: %s", name, e.what());
        }
    }
    return any;
}

// KeyList (the KEY BINDINGS page): every input goes to the port as PCBind(mId, FEInputId, MyMenu.MovedCursor)
void define_key_list(Asm &a)
{
    define_list_class(a, "KeyList", [](Asm &f) {
        input_id(f);
        f.logical_not();
        size_t none = f.branch(0x9D);
        trigger(f, "PCBind", {[](Asm &g) { g.push(Value::R(1)).push(Value::S("mId")).get_member(); }, input_id,
                              at({"MyMenu", "MovedCursor"})});
        f.land(none);
    });
}

// PagePrepare / PageRelease / PageDestruct for a page of rows Main...<clip prefix><i> (bar: the slider's clip)
struct PageScript {
    std::vector<RowSpec> rows;
    std::function<Path(size_t)> clip, bar;
    std::string choice_class; // "BkListBox"
    std::string back;         // page Back goes to
    int page_id;              // PCPage(id) when prepared: the port fills the page's values (a group, or 100: keys)
    bool wrap;                // up on the first row goes to the last (not on the key list: it scrolls)
    std::vector<std::function<void(Asm &)>> extra_prepare;
};

void page_script(Movie &mv, const PageScript &p)
{
    uint32_t script = mv.main_script();
    for (const char *f : {"PagePrepare", "PageRelease", "PageDestruct"}) mv.rename_function(script, f, std::string(f) + "0");
    bool debug = getenv("BLACK_FE_DEBUG") != nullptr;
    mv.rename_function(script, "Update", "Update0");
    bool keys = std::any_of(p.rows.begin(), p.rows.end(), [](const RowSpec &r) { return r.kind == RowSpec::Key; });
    Classes classes = own_classes_prepare(mv, keys ? Classes{"KeyList"} : Classes{});
    Asm a(mv);
    if (keys) define_key_list(a);
    own_classes_script(a, classes, std::to_string(p.page_id));
    size_t k = p.rows.size();
    auto row_var = [](size_t i) { return "pcRow" + std::to_string(i); };
    auto list_var = [](size_t i) { return "pcList" + std::to_string(i); };
    for (size_t i = 0; i < k; i++) {
        a.push(Value::S(row_var(i))).define_local2();
        a.push(Value::S(list_var(i))).define_local2();
    }
    std::vector<std::string> hover;
    std::vector<int> ids;
    for (size_t i = 0; i < k; i++) hover.push_back(row_var(i)), ids.push_back(p.rows[i].id);
    a.push(Value::S("pcOver")).define_local2(); // the row under the pointer, and the one it focused last
    a.push(Value::S("pcHover")).define_local2();
    a.function2("PagePrepare", [&](Asm &f) {
        for (size_t i = 0; i < k; i++) {
            const RowSpec &r = p.rows[i];
            Path clip = p.clip(i);
            if (r.kind == RowSpec::Link) {
                make(f, row_var(i), "BkMenuButton", {at(clip)});
                set(f, {row_var(i)}, "actionSelectPage", S(r.page));
                continue;
            }
            make(f, row_var(i), "BkMultiChoiceMenuButton", {at(clip)});
            if (r.kind == RowSpec::Slider) {
                make(f, list_var(i), "BkMovieclipSlider",
                     {at(p.bar(i)), S("PCSlider" + std::to_string(r.id)), at({"_global", "externObject", "FEPCS" + std::to_string(r.id)})});
            } else if (r.kind == RowSpec::Key) {
                make(f, list_var(i), "KeyList");
                set(f, {list_var(i)}, "mId", I(r.id));
                set(f, {row_var(i)}, "actionSelectFunction", nothing_fn()); // select is not Back
            } else if (p.choice_class == "BkListBox") {
                Path left = clip, right = clip;
                left.insert(left.end(), {"Arrows", "LeftArrow"});
                right.insert(right.end(), {"Arrows", "RightArrow"});
                make(f, list_var(i), "BkListBox");
                call(f, {list_var(i)}, "SetId", {I(r.id)});
                call(f, {list_var(i)}, "SetGameFunction", {S("PCChange")});
                call(f, {list_var(i)}, "SetLeftAnimation", {at(left)});
                call(f, {list_var(i)}, "SetRightAnimation", {at(right)});
            } else {
                make(f, list_var(i), p.choice_class);
                set(f, {list_var(i)}, "mId", I(r.id));
            }
            set(f, {row_var(i)}, "mChoiceList", at({list_var(i)}));
        }
        for (size_t i = 0; i < k; i++) {
            if (p.wrap || i + 1 < k) set(f, {row_var(i)}, "buttonDown", at({row_var((i + 1) % k)}));
            if (p.wrap || i > 0) set(f, {row_var(i)}, "buttonUp", at({row_var((i + k - 1) % k)}));
        }
        call(f, {"MyMenu"}, "SetFocusButton", {at({row_var(0)})});
        call(f, {"MyMenu"}, "SetPageBack", {S(p.back)});
        if (debug) f.push(Value::S("pcMenu")).push(Value::S("MyMenu")).get_var().set_var();
        for (const auto &e : p.extra_prepare) e(f);
        trigger(f, "PCPage", {I(p.page_id)});
        f.push_true().ret();
    });
    a.function2("PageRelease", [&](Asm &f) {
        for (size_t i = 0; i < k; i++) {
            call(f, {row_var(i)}, "Release");
            if (p.rows[i].kind != RowSpec::Link) call(f, {list_var(i)}, "Release");
        }
    });
    a.function2("Update", [&](Asm &f) {
        emit_heal(f, row_var(0), classes, std::to_string(p.page_id));
        call_function(f, "Update0");
        hover_block(f, hover, ids);
        // Keep a slider's knob on its setting: the port's own mouse click moves the setting (input/bindings.cpp
        // pcset::slide) without the game's widget, which would otherwise leave the knob where the pad last left it.
        for (size_t i = 0; i < k; i++) {
            if (p.rows[i].kind != RowSpec::Slider) continue;
            defined({list_var(i), "SetValue"})(f);
            size_t none = f.branch(0x9D);
            call(f, {list_var(i)}, "SetValue",
                 {at({"_global", "externObject", "FEPCS" + std::to_string(p.rows[i].id)})});
            f.land(none);
        }
        if (debug) { // tests: each input reports the page's state (PCDebug in black.log)
            at({"_global", "externObject", "FEInputId"})(f);
            f.logical_not();
            size_t none = f.branch(0x9D);
            auto concat = [](std::vector<Push> parts) {
                return [parts](Asm &g) {
                    g.push(Value::S(""));
                    for (const Push &x : parts) {
                        x(g);
                        g.add2().push(Value::S(" ")).add2();
                    }
                };
            };
            trigger(f, "PCDebug", {concat({S("in"), at({"_global", "externObject", "FEInputId"}), S("rel"), at({"_global", "externObject", "FERelease"})}),
                                   concat({S("focus"), at({"MyMenu", "mFocusButton", "movieClip", "_name"}), S("frame"), at({"Main", "_currentframe"})}),
                                   concat({S("inst"), at({"pcInst"}), S("own"), defined({own_var("BkMenuList")}), S("g"), defined({"_global", "BkMenuList"}), S("back"), at({"MyMenu", "mstrPageBack"})})});
            f.land(none);
            dbg_flush(f);
        }
    });
    a.function2("PageDestruct", [&](Asm &f) {
        for (size_t i = 0; i < k; i++) {
            f.push(Value::S(row_var(i))).delete2().pop();
            f.push(Value::S(list_var(i))).delete2().pop();
        }
    });
    mv.append_control(mv.main, 0, mv.do_action(a.finish()));
}

// ---------------------------------------------------------------------------------------------------- main menu
// A row added below the bottom row of a menu's TextAnim. The game's menus (Options, the main menu) place each row's
// text (character 5 in sprite 6 in sprite 7 for the bottom one, at depth 1) transparent in frame 0 and reveal it at a
// frame (a Move with a colour transform) with a flash bar. The copy: depth 2, dy lower, named `name`, revealed 3
// frames after the bottom row with its own bar at new_bar_depth.
void add_menu_row(Movie &mv, uint32_t textanim, const std::string &label, const std::string &name, float dy, int reveal,
                  uint32_t bar_depth, uint32_t new_bar_depth, uint32_t row_depth = 2)
{
    int t = mv.add_character(mv.clone_text(mv.character(5), label, label));
    int s6 = mv.add_character(mv.clone_sprite(mv.character(6), {{5, t}}));
    int s7 = mv.add_character(mv.clone_sprite(mv.character(7), {{6, s6}}));
    Bytes row = body(mv, mv.find_place(textanim, 0, 1));
    wr(row, 8, row_depth);
    wr(row, 12, (uint32_t)s7);
    wrf(row, 36, rdf(row, 36) + dy);
    wr(row, 52, mv.cstring(name));
    mv.append_control(textanim, 0, mv.alloc(row.data(), 64));
    Bytes show = body(mv, mv.find_place(textanim, reveal, 1, 1));
    wr(show, 8, row_depth);
    mv.append_control(textanim, reveal + 3, mv.alloc(show.data(), 64));
    Bytes bar = body(mv, mv.find_place(textanim, reveal, bar_depth));
    wr(bar, 8, new_bar_depth);
    wrf(bar, 36, rdf(bar, 36) + dy);
    mv.append_control(textanim, reveal + 3, mv.alloc(bar.data(), 64));
}

void options_row(Movie &mv)
{
    add_menu_row(mv, mv.character(27), lang::tr("PC SETTINGS"), "PCSettings_Text", 25.9f, 55, 21, 22); // below the Profile row

    uint32_t script = mv.main_script();
    for (const char *f : {"PagePrepare", "PageRelease", "PageDestruct", "Update"}) mv.rename_function(script, f, std::string(f) + "0");
    Classes classes = own_classes_prepare(mv);
    Asm a(mv);
    own_classes_script(a, classes, "opt");
    heal_update(a, "pcButton", classes, "opt",
                {"controllerButton", "soundButton", "pictureButton", "Playerprofile", "pcButton"});
    a.push(Value::S("pcButton")).define_local2();
    a.function2("PagePrepare", [](Asm &f) {
        call_function(f, "PagePrepare0");
        make(f, "pcButton", "BkMenuButton", {at({"Main", "TextAnim", "PCSettings_Text"})});
        set(f, {"Playerprofile"}, "buttonDown", at({"pcButton"}));
        set(f, {"pcButton"}, "buttonUp", at({"Playerprofile"}));
        set(f, {"pcButton"}, "buttonDown", at({"controllerButton"}));
        set(f, {"controllerButton"}, "buttonUp", at({"pcButton"}));
        set(f, {"pcButton"}, "actionSelectPage", S("PCSettings"));
        f.push_true().ret();
    });
    a.function2("PageRelease", [](Asm &f) {
        call_function(f, "PageRelease0");
        call(f, {"pcButton"}, "Release");
    });
    a.function2("PageDestruct", [](Asm &f) {
        call_function(f, "PageDestruct0");
        f.push(Value::S("pcButton")).delete2().pop();
    });
    mv.append_control(mv.main, 0, mv.do_action(a.finish()));
}

// The main menu (blacktitlemenu: CONTINUE MISSION / OPTIONS / OPERATOR PROFILE; blacktitlemenu2 with a new profile:
// START MISSION / OPTIONS) gets QUIT at the bottom, in the up / down ring between the last and the first button; it
// asks the port to close the game (PCQuit, like closing the window).
void main_menu_quit(Movie &mv, int textanim, float dy, int reveal, uint32_t bar_depth, uint32_t new_bar_depth,
                    const char *last, const char *first, const std::vector<std::string> &hover)
{
    add_menu_row(mv, mv.character(textanim), lang::tr("QUIT"), "Quit_Text", dy, reveal, bar_depth, new_bar_depth);
    uint32_t script = mv.main_script();
    for (const char *f : {"PagePrepare", "PageRelease", "PageDestruct", "Update"}) mv.rename_function(script, f, std::string(f) + "0");
    Classes classes = own_classes_prepare(mv);
    Asm a(mv);
    own_classes_script(a, classes, "main");
    heal_update(a, "quitButton", classes, "main", hover);
    a.push(Value::S("quitButton")).define_local2();
    a.function2("PagePrepare", [last, first](Asm &f) {
        call_function(f, "PagePrepare0");
        make(f, "quitButton", "BkMenuButton", {at({"Main", "TextAnim", "Quit_Text"})});
        set(f, {last}, "buttonDown", at({"quitButton"}));
        set(f, {"quitButton"}, "buttonUp", at({last}));
        set(f, {"quitButton"}, "buttonDown", at({first}));
        set(f, {first}, "buttonUp", at({"quitButton"}));
        set(f, {"quitButton"}, "actionSelectFunction", [](Asm &g) {
            g.function2("", [](Asm &h) { trigger(h, "PCQuit", {}); });
        });
        f.push_true().ret();
    });
    a.function2("PageRelease", [](Asm &f) {
        call_function(f, "PageRelease0");
        call(f, {"quitButton"}, "Release");
    });
    a.function2("PageDestruct", [](Asm &f) {
        call_function(f, "PageDestruct0");
        f.push(Value::S("quitButton")).delete2().pop();
    });
    mv.append_control(mv.main, 0, mv.do_action(a.finish()));
}

void pc_menu(Movie &mv)
{
    const auto &groups = pcset::groups();
    if (groups.size() != 4) throw Error("the PC SETTINGS menu has four rows");
    literal(mv, mv.character(18), lang::tr("PC SETTINGS"));
    const int texts[4] = {14, 11, 8, 5}; // rows top to bottom: ControllerSetup, SoundSetup, PictureSetup, Playerprofile
    for (int i = 0; i < 4; i++) literal(mv, mv.character(texts[i]), groups[i].title);
    // MODS: a fifth row below the four, opening the mods page (mods.h, pcset::mods_group). Same movie and same
    // measurements as the "PC SETTINGS" row the port adds to the game's own Options page.
    add_menu_row(mv, mv.character(27), lang::tr("MODS"), "Mods_Text", 25.9f, 55, 21, 22);
    // INPUT: a sixth row below MODS (pcset::input_group), revealed with it (the movie has no later reveal frame)
    add_menu_row(mv, mv.character(27), lang::tr("INPUT"), "Input_Text", 2 * 25.9f, 55, 21, 61, 60); // depths the movie leaves free
    mv.rename_function(mv.main_script(), "PagePrepare", "PagePrepare0");
    mv.rename_function(mv.main_script(), "Update", "Update0");
    Classes classes = own_classes_prepare(mv);
    Asm a(mv);
    own_classes_script(a, classes, "pcs");
    heal_update(a, "controllerButton", classes, "pcs");
    // the rows this page shows, top to bottom, as the pointer finds them: the game's own four and the port's MODS row
    heal_update(a, "modsButton", classes, "pcs",
                {"controllerButton", "soundButton", "pictureButton", "Playerprofile", "modsButton", "inputButton"});
    a.push(Value::S("modsButton")).define_local2();
    a.push(Value::S("inputButton")).define_local2();
    a.push(Value::S("pcOver")).define_local2(); // the row under the pointer, and the one it focused last
    a.push(Value::S("pcHover")).define_local2();
    a.function2("PagePrepare", [&groups](Asm &f) {
        call_function(f, "PagePrepare0");
        const char *buttons[4] = {"controllerButton", "soundButton", "pictureButton", "Playerprofile"};
        for (int i = 0; i < 4; i++) set(f, {buttons[i]}, "actionSelectPage", S(groups[i].page));
        make(f, "modsButton", "BkMenuButton", {at({"Main", "TextAnim", "Mods_Text"})});
        make(f, "inputButton", "BkMenuButton", {at({"Main", "TextAnim", "Input_Text"})});
        set(f, {"Playerprofile"}, "buttonDown", at({"modsButton"}));
        set(f, {"modsButton"}, "buttonUp", at({"Playerprofile"}));
        set(f, {"modsButton"}, "buttonDown", at({"inputButton"}));
        set(f, {"inputButton"}, "buttonUp", at({"modsButton"}));
        set(f, {"inputButton"}, "buttonDown", at({"controllerButton"}));
        set(f, {"controllerButton"}, "buttonUp", at({"inputButton"}));
        set(f, {"modsButton"}, "actionSelectPage", S(pcset::mods_group().page));
        set(f, {"inputButton"}, "actionSelectPage", S(pcset::input_group().page));
        call(f, {"MyMenu"}, "SetFunctionBack"); // no argument: not Options' own back (save and leave)
        call(f, {"MyMenu"}, "SetPageBack", {S("OptionsMenuXbox")});
        f.push_true().ret();
    });
    a.function2("PageRelease", [](Asm &f) {
        call_function(f, "PageRelease0");
        call(f, {"modsButton"}, "Release");
        call(f, {"inputButton"}, "Release");
    });
    a.function2("PageDestruct", [](Asm &f) {
        call_function(f, "PageDestruct0");
        f.push(Value::S("modsButton")).delete2().pop();
        f.push(Value::S("inputButton")).delete2().pop();
    });
    mv.append_control(mv.main, 0, mv.do_action(a.finish()));
}

struct Slot {
    uint32_t depth;
    int reveal;
    uint32_t bar;
};
const Slot kSlots[] = {{227, 43, 229}, {123, 46, 231}, {121, 49, 233}, {17, 52, 235}, {9, 55, 237}, {1, 58, 239}};
constexpr float kRowY0 = -108.8f, kRowDY = 40.0f;
// the bullet bar at 45 % (a bullet is about 33 units tall) on the value line, centred about 16 below the row
constexpr float kBarScale = 0.45f, kBarDX = 12.0f, kBarDY = 16.0f - 15.9f * kBarScale, kNumberX = 140.0f;
constexpr float kKeysX = 175.0f; // a key row's keys, on its caption's line
constexpr float kValueRight = 265.0f; // a choice's value box ends by the right arrow
constexpr uint32_t kBarDepth = 101;

void choice_page(Movie &mv, const std::string &title, const std::vector<RowSpec> &rows, const std::string &back, bool wrap, int page_id)
{
    if (rows.size() > 6) throw Error("a page has at most six rows");
    uint32_t textanim = mv.character(49);
    literal(mv, mv.character(42), title);
    Bytes proto = body(mv, mv.find_place(textanim, 0, 1)); // PlayModeButton: a choice row
    std::vector<uint32_t> frame0 = mv.controls(textanim, 0);
    for (size_t i = 0; i < 6; i++) {
        const Slot &s = kSlots[i];
        uint32_t old = mv.find_place(textanim, 0, s.depth);
        size_t at = std::find(frame0.begin(), frame0.end(), old) - frame0.begin(); // an index: frame0 grows below
        if (i >= rows.size()) { // unused slot: no row, no reveal, no bar
            frame0.erase(frame0.begin() + at);
            std::vector<uint32_t> keep;
            for (uint32_t c : mv.controls(textanim, s.reveal))
                if (rd(mv.apt, c) != 3 || (rd(mv.apt, c + 8) != s.depth && rd(mv.apt, c + 8) != s.bar)) keep.push_back(c);
            mv.set_controls(textanim, s.reveal, keep);
            continue;
        }
        const RowSpec &r = rows[i];
        float y = kRowY0 + kRowDY * i;
        int vs = -1;
        if (!r.value.empty()) {
            // the value's box (left, top, right, bottom at +8..+20) runs to the right arrow instead of 175: a longer
            // value ("PORTUGUÊS (BR) (NEXT START)") wraps, and only its first line shows
            uint32_t box = mv.clone_text(mv.character(21), r.value, r.value);
            wrf(mv.apt, box + 16, kValueRight);
            int vt = mv.add_character(box);
            vs = mv.add_character(mv.clone_sprite(mv.character(22), {{21, vt}}));
        }
        int row;
        if (r.kind == RowSpec::Choice) {
            int lt = mv.add_character(mv.clone_text(mv.character(23), r.label, r.label));
            int ls = mv.add_character(mv.clone_sprite(mv.character(24), {{23, lt}}));
            row = mv.add_character(mv.clone_sprite(mv.character(25), {{24, ls}, {22, vs}}));
        } else {
            int lt = mv.add_character(mv.clone_text(mv.character(38), r.label, r.label));
            int ls = mv.add_character(mv.clone_sprite(mv.character(39), {{38, lt}}));
            uint32_t rc = mv.clone_sprite(mv.character(40), {{39, ls}});
            if (r.kind == RowSpec::Slider) mv.append_control(rc, 0, mv.place(3, vs, 0x06, kNumberX, 16.5f));
            if (r.kind == RowSpec::Key) mv.append_control(rc, 0, mv.place(3, vs, 0x06, kKeysX, -1.2f));
            row = mv.add_character(rc);
        }
        if (r.kind == RowSpec::Slider) {
            Bytes bar = body(mv, mv.find_place(textanim, 0, 123)); // Sound_Volume
            wr(bar, 8, kBarDepth + 2 * (uint32_t)i);
            wrf(bar, 16, kBarScale);
            wrf(bar, 20, 0);
            wrf(bar, 24, 0);
            wrf(bar, 28, kBarScale);
            wrf(bar, 32, rdf(proto, 32) + kBarDX);
            wrf(bar, 36, y + kBarDY);
            wr(bar, 52, mv.cstring("Slider" + std::to_string(i)));
            frame0.push_back(mv.alloc(bar.data(), 64));
            Bytes show = body(mv, mv.find_place(textanim, s.reveal, s.depth, 1));
            wr(show, 8, kBarDepth + 2 * (uint32_t)i);
            mv.append_control(textanim, s.reveal, mv.alloc(show.data(), 64));
        }
        Bytes b = proto;
        wr(b, 8, s.depth);
        wr(b, 12, (uint32_t)row);
        wrf(b, 36, y);
        wr(b, 52, mv.cstring("Row" + std::to_string(i)));
        frame0[at] = mv.alloc(b.data(), 64);
        wrf(mv.apt, mv.find_place(textanim, s.reveal, s.bar) + 36, y);
    }
    mv.set_controls(textanim, 0, frame0);

    PageScript p;
    p.rows = rows;
    p.clip = [](size_t i) { return Path{"Main", "TextAnim", "Row" + std::to_string(i)}; };
    p.bar = [](size_t i) { return Path{"Main", "TextAnim", "Slider" + std::to_string(i)}; };
    p.choice_class = "BkListBox";
    p.back = back;
    p.wrap = wrap;
    p.page_id = page_id;
    p.extra_prepare.push_back([](Asm &f) { // the Options background movie, as Sound Setup
        set(f, {"_global", "externObject"}, "FECurrentFmv", I(2));
        set(f, {"_global", "externObject"}, "FELoopFmv", I(1));
    });
    page_script(mv, p);
}

// ---------------------------------------------------------------------------------------------------- pause menu
// In play the front end runs InGameOn.bin (pausemenu: CONTINUE ... QUIT) and loads PseMenu.bin for the pause pages
// (pausemenuoptionxbox: the in-game Options). Their rows sit straight in a Main clip that slides in: frames 1-29
// move every row (Move + matrix) to its place, so a new row copies an existing row's moves. Rows are single lines:
// caption, then the value (or the bullet bar and the number) to the right.

// the moves of a depth (frames after 0)
std::vector<std::pair<int, Bytes>> moves_of(const Movie &mv, uint32_t sprite, uint32_t depth)
{
    std::vector<std::pair<int, Bytes>> out;
    for (int f = 1; f < (int)rd(mv.apt, sprite + 8); f++)
        for (uint32_t c : mv.controls(sprite, f))
            if (rd(mv.apt, c) == 3 && rd(mv.apt, c + 8) == depth && (rd(mv.apt, c + 4) & 1)) out.push_back({f, body(mv, c)});
    return out;
}

void add_moves(Movie &mv, uint32_t sprite, const std::vector<std::pair<int, Bytes>> &moves, uint32_t depth, float dy)
{
    for (const auto &[f, m] : moves) {
        Bytes b = m;
        wr(b, 8, depth);
        if (rd(b, 4) & 4) wrf(b, 36, rdf(b, 36) + dy);
        mv.append_control(sprite, f, mv.alloc(b.data(), 64));
    }
}

void shift_depth(Movie &mv, uint32_t sprite, uint32_t depth, float dy)
{
    for (int f = 0; f < (int)rd(mv.apt, sprite + 8); f++)
        for (uint32_t c : mv.controls(sprite, f))
            if (rd(mv.apt, c) == 3 && rd(mv.apt, c + 8) == depth && (rd(mv.apt, c + 4) & 4)) wrf(mv.apt, c + 36, rdf(mv.apt, c + 36) + dy);
}

void remove_depths(Movie &mv, uint32_t sprite, std::initializer_list<uint32_t> depths)
{
    for (int f = 0; f < (int)rd(mv.apt, sprite + 8); f++) {
        std::vector<uint32_t> keep, all = mv.controls(sprite, f);
        for (uint32_t c : all)
            if (rd(mv.apt, c) != 3 || std::find(depths.begin(), depths.end(), rd(mv.apt, c + 8)) == depths.end()) keep.push_back(c);
        if (keep.size() != all.size()) mv.set_controls(sprite, f, keep);
    }
}

void require_free(const Movie &mv, uint32_t sprite, uint32_t depth)
{
    for (int f = 0; f < (int)rd(mv.apt, sprite + 8); f++)
        for (uint32_t c : mv.controls(sprite, f))
            if (rd(mv.apt, c) == 3 && rd(mv.apt, c + 8) == depth) throw Error("depth " + std::to_string(depth) + " is in use");
}

// pausemenu: PC SETTINGS after OPTIONS (Main char 33; rows CONTINUE 89, OBJECTIVES STATUS 77, RESTART 87, OPTIONS 85
// and QUIT 83, 25 apart): the four rows above QUIT move up a row, the new one takes OPTIONS' place and moves
void pause_row(Movie &mv)
{
    uint32_t main_clip = mv.character(33);
    constexpr uint32_t kDepth = 97;
    require_free(mv, main_clip, kDepth);
    int lt = mv.add_character(mv.clone_text(mv.character(18), lang::tr("PC SETTINGS"), lang::tr("PC SETTINGS")));
    int ls = mv.add_character(mv.clone_sprite(mv.character(19), {{18, lt}}));
    int row = mv.add_character(mv.clone_sprite(mv.character(20), {{19, ls}}));
    Bytes b = body(mv, mv.find_place(main_clip, 0, 85));
    wr(b, 8, kDepth);
    wr(b, 12, (uint32_t)row);
    wr(b, 52, mv.cstring("PCSettings"));
    auto moves = moves_of(mv, main_clip, 85);
    mv.append_control(main_clip, 0, mv.alloc(b.data(), 64));
    add_moves(mv, main_clip, moves, kDepth, 0);
    for (uint32_t d : {89u, 77u, 87u, 85u}) shift_depth(mv, main_clip, d, -25.0f);

    uint32_t script = mv.main_script();
    for (const char *f : {"PagePrepare", "PageRelease", "PageDestruct", "Update"}) mv.rename_function(script, f, std::string(f) + "0");
    Classes classes = own_classes_prepare(mv);
    Asm a(mv);
    own_classes_script(a, classes, "pause");
    heal_update(a, "pcButton", classes, "pause",
                {"continueButton", "objectivesButton", "restartButton", "optionsButton", "pcButton", "quitButton"});
    a.push(Value::S("pcButton")).define_local2();
    a.function2("PagePrepare", [](Asm &f) {
        call_function(f, "PagePrepare0");
        make(f, "pcButton", "BkMenuButton", {at({"Main", "PCSettings"})});
        set(f, {"optionsButton"}, "buttonDown", at({"pcButton"}));
        set(f, {"pcButton"}, "buttonUp", at({"optionsButton"}));
        set(f, {"pcButton"}, "buttonDown", at({"quitButton"}));
        set(f, {"quitButton"}, "buttonUp", at({"pcButton"}));
        set(f, {"pcButton"}, "actionSelectPage", S(ingame("PCPage")));
        f.push_true().ret();
    });
    a.function2("PageRelease", [](Asm &f) {
        call_function(f, "PageRelease0");
        call(f, {"pcButton"}, "Release");
    });
    a.function2("PageDestruct", [](Asm &f) {
        call_function(f, "PageDestruct0");
        f.push(Value::S("pcButton")).delete2().pop();
    });
    mv.append_control(mv.main, 0, mv.do_action(a.finish()));
}

// the PC SETTINGS pause page (fe/pause_ui.cpp): one copy of pausemenuoptionxbox (Main char 50; the Vibration row 79
// at y -33.8 is the pattern) whose six rows show what the port puts in their values. Its own rows (toggles 75-91
// with ON/OFF texts, sliders 93/199 and their captions 197/303) go.
constexpr float kIgRowY0 = -87.9f, kIgRowDY = 27.0f, kIgPatternY = -33.8f;
constexpr float kIgValueX = 205.0f, kIgBarX = 185.0f, kIgBarScale = 0.5f, kIgNumberX = 313.0f;
constexpr float kIgBarY = -15.3f * kIgBarScale; // the bar's middle is 15.3 below its origin, the text's at the origin

std::string var(int n) { return "&PC_V" + std::to_string(n); }

// row i's packed state externObject.FEPCR<i>, shifted right and masked: pushes (R >> shift) & mask
Push row_bits(size_t i, int shift, int mask)
{
    return [i, shift, mask](Asm &f) {
        at({"_global", "externObject", "FEPCR" + std::to_string(i)})(f);
        if (shift) f.push(Value::I(shift)).op(0x64); // BitRShift
        f.push(Value::I(mask)).op(0x60);             // BitAnd
    };
}

// pcRows[((R >> shift) & 15) - 1]: the row a link points at (index -1: undefined, no row)
Push row_link(size_t i, int shift)
{
    return [i, shift](Asm &f) {
        at({"pcRows"})(f);
        row_bits(i, shift, 15)(f);
        f.push(Value::I(1)).op(0x0B); // Subtract
        f.get_member();
    };
}

void pause_page(Movie &mv)
{
    using pauseui::kRows;
    uint32_t main_clip = mv.character(50);
    literal(mv, mv.character(47), var(fevar::kTitle));
    Bytes proto = body(mv, mv.find_place(main_clip, 0, 79));
    auto moves = moves_of(mv, main_clip, 79);
    remove_depths(mv, main_clip, {75, 77, 79, 81, 83, 85, 87, 89, 91, 93, 197, 199, 303});
    for (int i = 0; i < kRows; i++) {
        float y = kIgRowY0 + kIgRowDY * i;
        uint32_t depth = 75 + 2 * (uint32_t)i;
        std::string label = var(fevar::kLabel + i);
        int lt = mv.add_character(mv.clone_text(mv.character(18), label, label));
        int ls = mv.add_character(mv.clone_sprite(mv.character(19), {{18, lt}}));
        uint32_t rc = mv.clone_sprite(mv.character(20), {{19, ls}});
        uint32_t text_depth = 3;
        for (auto [n, x] : {std::pair<int, float>{fevar::kValue + i, kIgValueX}, {fevar::kNumber + i, kIgNumberX}}) {
            int vt = mv.add_character(mv.clone_text(mv.character(15), var(n), var(n)));
            int vs = mv.add_character(mv.clone_sprite(mv.character(16), {{15, vt}}));
            mv.append_control(rc, 0, mv.place(text_depth, vs, 0x06, x, 0.1f));
            text_depth += 2;
        }
        mv.append_control(rc, 0, mv.place(text_depth, 39, 0x06, kIgBarX, kIgBarY, kIgBarScale, "Bar")); // the bullet bar
        int row = mv.add_character(rc);
        Bytes b = proto;
        wr(b, 8, depth);
        wr(b, 12, (uint32_t)row);
        wrf(b, 36, y);
        wr(b, 52, mv.cstring("Row" + std::to_string(i)));
        mv.append_control(main_clip, 0, mv.alloc(b.data(), 64));
        add_moves(mv, main_clip, moves, depth, y - kIgPatternY);
    }

    uint32_t script = mv.main_script();
    for (const char *f : {"PagePrepare", "PageRelease", "PageDestruct", "Update"}) mv.rename_function(script, f, std::string(f) + "0");
    Classes classes = own_classes_prepare(mv, {"PcRow"});
    Asm a(mv);
    // PcRow: inputs 1-5 (not 0: none, not 6: Back, which reports itself) go to the port as PCRow(mId, input, moved)
    define_list_class(a, "PcRow", [](Asm &f) {
        input_id(f);
        f.push(Value::I(5)).op(0x67); // Greater
        size_t skip1 = f.branch(0x9D);
        input_id(f);
        f.logical_not();
        size_t skip2 = f.branch(0x9D);
        trigger(f, "PCRow", {[](Asm &g) { g.push(Value::R(1)).push(Value::S("mId")).get_member(); }, input_id,
                             at({"MyMenu", "MovedCursor"})});
        f.land(skip1);
        f.land(skip2);
    });
    own_classes_script(a, classes, "pp");
    auto row_var = [](size_t i) { return "pcRow" + std::to_string(i); };
    auto list_var = [](size_t i) { return "pcList" + std::to_string(i); };
    auto clip = [](size_t i) { return Path{"Main", "Row" + std::to_string(i)}; };
    for (size_t i = 0; i < kRows; i++) {
        a.push(Value::S(row_var(i))).define_local2();
        a.push(Value::S(list_var(i))).define_local2();
    }
    a.push(Value::S("pcRows")).define_local2();
    a.push(Value::S("pcLayout")).define_local2();
    a.push(Value::S("pcOver")).define_local2(); // the row under the pointer, and the one it focused last
    a.push(Value::S("pcHover")).define_local2();
    a.function2("PagePrepare", [&](Asm &f) {
        for (size_t i = 0; i < kRows; i++) {
            make(f, row_var(i), "BkMultiChoiceMenuButton", {at(clip(i))});
            make(f, list_var(i), "PcRow");
            set(f, {list_var(i)}, "mId", I((int)i));
            set(f, {row_var(i)}, "mChoiceList", at({list_var(i)}));
            set(f, {row_var(i)}, "actionSelectFunction", nothing_fn()); // select is not Back
        }
        f.push(Value::S("pcRows"));
        for (size_t i = kRows; i-- > 0;) at({row_var(i)})(f);
        f.push(Value::I(kRows)).op(0x42).set_var(); // InitArray
        f.push({Value::S("pcLayout"), Value::I(-1)}).set_var();
        call(f, {"MyMenu"}, "SetFunctionBack", {[](Asm &g) {
                 g.function2("", [](Asm &h) { // at the top screen Back leaves for the pause menu, below it goes up
                     at({"_global", "externObject", "FEPCAtRoot"})(h);
                     h.logical_not();
                     size_t below = h.branch(0x9D);
                     set(h, {"_global", "externObject"}, "FETargetPage", S("PauseMenu"));
                     set(h, {"_global", "externObject"}, "FEChangePage", I(1));
                     size_t done = h.branch(0x99);
                     h.land(below);
                     trigger(h, "PCBack", {});
                     h.land(done);
                 });
             }});
        call(f, {"MyMenu"}, "SetFocusButton", {at({row_var(0)})});
        trigger(f, "PCOpen", {at({"Main", "Row0", "Bar", "_totalframes"})});
        f.push_true().ret();
    });
    a.function2("Update", [&](Asm &f) {
        emit_heal(f, row_var(0), classes, "pp");
        call_function(f, "Update0");
        dbg_flush(f);
        at({"_global", "externObject", "FEPCLayout"})(f); // a new screen: rows shown, linked, focused
        at({"pcLayout"})(f);
        f.equals2();
        size_t same = f.branch(0x9D);
        f.push(Value::S("pcLayout"));
        at({"_global", "externObject", "FEPCLayout"})(f);
        f.set_var();
        for (size_t i = 0; i < kRows; i++) {
            set(f, clip(i), "_visible", row_bits(i, 0, 1));
            set(f, {row_var(i)}, "buttonDown", row_link(i, 16));
            set(f, {row_var(i)}, "buttonUp", row_link(i, 20));
        }
        call(f, {"MyMenu"}, "SetFocusButton", {[](Asm &g) {
                 at({"pcRows"})(g);
                 at({"_global", "externObject", "FEPCFocus"})(g);
                 g.get_member();
             }});
        f.land(same);
        for (size_t i = 0; i < kRows; i++) { // the bars, every update
            Path bar = clip(i);
            bar.push_back("Bar");
            set(f, bar, "_visible", row_bits(i, 1, 1));
            call(f, bar, "gotoAndStop", {row_bits(i, 8, 255)});
        }
        std::vector<std::string> hover;
        for (size_t i = 0; i < kRows; i++) hover.push_back(row_var(i));
        hover_block(f, hover, {});
    });
    a.function2("PageRelease", [&](Asm &f) {
        for (size_t i = 0; i < kRows; i++) {
            call(f, {row_var(i)}, "Release");
            call(f, {list_var(i)}, "Release");
        }
    });
    a.function2("PageDestruct", [&](Asm &f) {
        for (size_t i = 0; i < kRows; i++) {
            f.push(Value::S(row_var(i))).delete2().pop();
            f.push(Value::S(list_var(i))).delete2().pop();
        }
        f.push(Value::S("pcRows")).delete2().pop();
    });
    mv.append_control(mv.main, 0, mv.do_action(a.finish()));
}

// ---------------------------------------------------------------------------------------------------- files
bool read_file(const std::wstring &path, Bytes &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool write_file(const std::wstring &path, const Bytes &data)
{
    for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1))
        CreateDirectoryW(path.substr(0, i).c_str(), nullptr); // each folder on the way
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char *)data.data(), (std::streamsize)data.size());
    f.close();
    if (!f) logf("fe: cannot write %s", narrow(path).c_str());
    return (bool)f;
}

// one container: read it, let edit() change it, keep the result; false (logged) if it does not fit
bool rebuild(const std::wstring &game_dir, const wchar_t *file, const std::function<void(Container &)> &edit, Bytes &out)
{
    std::wstring path = game_dir + L"\\export\\frontend\\" + file;
    Bytes in;
    if (!read_file(path, in)) {
        logf("fe: %s not found", narrow(path).c_str());
        return false;
    }
    try {
        Container c;
        c.parse(in);
        edit(c);
        out = c.build();
        return true;
    } catch (const std::exception &e) {
        logf("fe: %s is not laid out as expected (%s)", narrow(path).c_str(), e.what());
        return false;
    }
}

// Controller button icons in the game's text are private-use characters the game's font draws as Xbox buttons
// (UTF-8 EF 80 xx = U+F0xx). In the button prompts ("<icon> to reload", "Press <icon> to crouch") each names the
// button of an action in the default controller layout; it becomes the key bound to that action, as "[R]". Text
// is rebuilt at every start, so a binding changed in the game shows in the prompts from the next start.
int button_glyphs_to_keys(uint32_t string_key, std::string &t)
{
    struct Glyph {
        uint8_t code; // U+F0xx
        int action;   // index into bindings::actions()
    };
    static const Glyph kGlyphs[] = {
        {0x09, 4},  // right trigger: fire
        {0x0B, 6},  // A: reload
        {0x0C, 8},  // left stick click: crouch
        {0x0E, 11}, // D-pad right: next weapon
        {0x0F, 12}, // D-pad left: previous weapon
        {0x10, 5},  // right stick click: aim
        {0x1B, 9},  // left trigger: grenade
        {0x1E, 13}, // D-pad up: firing mode
        {0x1F, 14}, // Y: suppressor
        {0x20, 10}, // B: melee
        {0x21, 15}, // D-pad down: health pack
    };
    int n = 0;
    // The controller options' ON / OFF pair stands side by side; the Portuguese "Ligado" / "  Desligado" are too long
    // for it and overlap, so they read as one word. Short words like the original's.
    if (string_key == 0x11c3b459 && t == "Ligado") t = "Sim", n++;
    if (string_key == 0x45c04449 && t.find("Desligado") != std::string::npos) t = "N\xC3\xA4o", n++;
    if (g_cfg.input_pad) return n; // INPUT: CONTROLLER keeps the game's button icons
    // "Press START" before the main menu (and on later prompts): the port's menus take Enter. The Portuguese
    // translation says "o botäo start" (its font draws ä as ã).
    if (string_key == 0x905e4ee0 || string_key == 0x2fcce936 || string_key == 0x661a3a37) {
        for (auto [from, to] : {std::pair<const char *, const char *>{"o bot\xC3\xA4o start", "a tecla ENTER"}, {"START", "ENTER"},
                                {"Start", "ENTER"}, {"start", "ENTER"}})
            for (size_t p = t.find(from); p != std::string::npos; p = t.find(from, p + strlen(to))) {
                t.replace(p, strlen(from), to);
                n++;
            }
    }
    // The face buttons' one-character icons in menu text ("press <A> to save": U+00B2 = A, U+00B9 = B): the menus
    // take Enter and Backspace. Lines ending in ':' are the controller screen's button list and keep them.
    size_t last = t.find_last_not_of(" \r\n");
    if (last == std::string::npos || t[last] != ':')
        for (auto [from, to] : {std::pair<const char *, const char *>{"\xC2\xB2", "[ENTER]"}, {"\xC2\xB9", "[BACKSPACE]"}})
            for (size_t p = t.find(from); p != std::string::npos; p = t.find(from, p)) {
                t.replace(p, 2, to);
                p += strlen(to);
                n++;
            }
    for (size_t p = t.find("\xEF\x80"); p != std::string::npos && p + 2 < t.size(); p = t.find("\xEF\x80", p)) {
        uint8_t code = (uint8_t)t[p + 2] & 0x3F;
        std::string key;
        if (code == 0x0B && string_key == 0xd06fd1c1) key = "ENTER"; // "<A> to continue" after a hint
        else
            for (const Glyph &g : kGlyphs)
                if (g.code == code) key = bindings::hud_action_key(g.action);
        if (key.empty()) { // not a known prompt button, or the action has no key: keep the icon
            p += 3;
            continue;
        }
        key = "[" + key + "]";
        t.replace(p, 3, key);
        p += key.size();
        n++;
    }
    return n;
}

// The menus' button legend at the bottom of the screen (Core.bin, movie "corexbox"): SELECT with the D-pad's arrows
// (sprites 14, 15, 16: arrows char 10 around char 8), CONFIRM with the A button (22: "Button" = sprite 19) and BACK
// with B (28: sprite 25); each places its label sprite as "Text" at x 16 (the text field 2 / -9.3 inside it). The
// icons are scaled to nothing, a text with the key the port's menus take goes where they were, and the label moves
// right past it.
void legend_keys(Movie &mv)
{
    constexpr float kCharW = 9.5f, kKeyX = -8.0f, kGap = 8.0f; // rough width of one character; where the key text starts
    struct Item {
        int sprite, text_char;
        const char *key;
    };
    const char *arrows = lang::tr("[ARROWS]");
    const Item items[] = {{14, 12, arrows}, {15, 12, arrows}, {16, 12, arrows}, {22, 20, "[ENTER]"}, {28, 26, "[BACKSPACE]"}};
    for (const Item &it : items) {
        uint32_t sp = mv.character(it.sprite);
        bool label = false;
        for (uint32_t ctl : mv.controls(sp, 0)) {
            if (rd(mv.apt, ctl) != 3) continue;
            uint32_t nm = rd(mv.apt, ctl + 52);
            if (nm && mv.str(nm) == "Text") {
                float chars = 0; // characters (not UTF-8 bytes); W and M about twice as wide
                for (const char *c = it.key; *c; c++)
                    if (((uint8_t)*c & 0xC0) != 0x80) chars += *c == 'W' || *c == 'M' ? 2.0f : 1.0f;
                wrf(mv.apt, ctl + 32, kKeyX + kCharW * chars + kGap);
                label = true;
            } else {
                for (uint32_t m = 16; m <= 28; m += 4) wrf(mv.apt, ctl + m, 0.0f); // a b c d: nothing left to draw
            }
        }
        if (!label) throw Error("legend sprite " + std::to_string(it.sprite) + " has no Text");
        int t = mv.add_character(mv.clone_text(mv.character(it.text_char), it.key, it.key));
        mv.append_control(sp, 0, mv.place(31, t, 6, kKeyX + 2.0f, -9.4f));
    }
}

// The game's text (language\strings\MainUS.bin: a magic word, a word, the count n, the table offset 16; n offsets,
// ascending; each entry a 4-byte key (entries sorted by key) and a NUL-terminated string, padded to 4 bytes except the
// last): XBOX in its messages (saving and loading, free blocks, the hard disk check) reads COMPUTER instead, "XBOX
// CONSOLE" just COMPUTER. Keys and order stay; only the offsets move.
bool patch_strings(const std::wstring &game_dir, const std::wstring &out_root)
{
    const std::wstring rel = L"\\language\\strings\\MainUS.bin";
    // LANGUAGE: the Portuguese text ships with the port (lang.h); English is the game's own
    std::wstring src = game_dir + rel;
    if (lang::portuguese()) {
        std::wstring pt = lang::root() + L"\\strings\\MainUS.bin";
        if (GetFileAttributesW(pt.c_str()) != INVALID_FILE_ATTRIBUTES) src = pt;
        else logf("fe: %s not found; the game's own text is used", narrow(pt).c_str());
    }
    Bytes in;
    if (!read_file(src, in)) {
        logf("fe: %s not found", narrow(src).c_str());
        return false;
    }
    // XBOX in the messages: the computer's, in the language played
    const char *computer = lang::portuguese() ? "COMPUTADOR" : "COMPUTER";
    const char *console = lang::portuguese() ? "CONSOLE XBOX" : "XBOX CONSOLE";
    try {
        if (in.size() < 16) throw Error("too short");
        uint32_t n = rd(in, 8), table = rd(in, 12);
        if (table != 16 || table + 4ull * n > in.size()) throw Error("no string table");
        Bytes out(in.begin(), in.begin() + table + 4 * n);
        int changed = 0, prompts = 0, prompt_strings = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t at = rd(in, table + 4 * i), end = i + 1 < n ? rd(in, table + 4 * (i + 1)) : (uint32_t)in.size();
            if (at + 4 > end || end > in.size()) throw Error("an entry out of order");
            std::string s((const char *)&in[at + 4], strnlen((const char *)&in[at + 4], end - at - 4)), t = s;
            for (auto [from, to] : {std::pair<const char *, const char *>{console, computer}, {"XBOX", computer}})
                for (size_t p = t.find(from); p != std::string::npos; p = t.find(from, p + strlen(to)))
                    t.replace(p, strlen(from), to);
            changed += t != s;
            std::string u = t;
            prompts += button_glyphs_to_keys(rd(in, at), t);
            prompt_strings += t != u;
            wr(out, table + 4 * i, (uint32_t)out.size());
            out.insert(out.end(), in.begin() + at, in.begin() + at + 4);
            out.insert(out.end(), t.begin(), t.end());
            out.push_back(0);
            while (i + 1 < n && out.size() % 4) out.push_back(0);
        }
        if (!write_file(out_root + rel, out)) return false;
        logf("fe: game text (%s): %d strings say %s instead of XBOX", lang::code(g_cfg.language), changed, computer);
        logf("fe: game text: %d controller button icons in %d strings show the bound keys", prompts, prompt_strings);
        return true;
    } catch (const std::exception &e) {
        logf("fe: %s is not laid out as expected (%s)", narrow(src).c_str(), e.what());
        DeleteFileW((out_root + rel).c_str());
        return false;
    }
}

const Bytes &movie(const Container &c, const char *name)
{
    auto it = c.movies.find(name);
    if (it == c.movies.end()) throw Error(std::string(name) + " missing");
    return it->second;
}

void add_movie(Container &c, const std::string &name, Bytes data)
{
    c.movies[name] = std::move(data);
    c.order.push_back(name);
}

constexpr char kBindings[] = "PCBindings";

} // namespace

bool fe_build_pages(const std::wstring &game_dir, const std::wstring &out_root)
{
    const std::wstring dir = out_root + L"\\export\\frontend\\";
    // the main menu: Options gets PC SETTINGS, which opens a copy of Options, whose rows open copies of Sound Setup
    Bytes fe;
    bool main_ok = rebuild(game_dir, L"FEMain.bin", [](Container &c) {
        const Bytes options = movie(c, "optionsmenuxbox"), sound = movie(c, "soundsetupxbox");
        Movie menu(options);
        pc_menu(menu);
        add_movie(c, "pcsettings", menu.build());
        std::string controls;
        int index = 0;
        for (const pcset::Group &g : pcset::groups()) {
            Movie page(sound);
            choice_page(page, g.title, group_rows(g, false), "PCSettings", true, index++);
            add_movie(c, lower(g.page), page.build());
            for (int n : g.rows)
                if (pcset::link(n)) controls = g.page;
        }
        Movie keys(sound);
        choice_page(keys, lang::tr("KEY BINDINGS"), key_rows(), controls, false, 100);
        add_movie(c, lower(kBindings), keys.build());
        const pcset::Group &mg = pcset::mods_group(); // the MODS row on the PC SETTINGS page opens this one
        Movie mods(sound);
        choice_page(mods, mg.title, group_rows(mg, false), "PCSettings", true, 4);
        add_movie(c, lower(mg.page), mods.build());
        const pcset::Group &ig = pcset::input_group(); // and INPUT, the row below it
        Movie input(sound);
        choice_page(input, ig.title, group_rows(ig, false), "PCSettings", true, 5);
        add_movie(c, lower(ig.page), input.build());
        Movie opt(options);
        options_row(opt);
        c.movies["optionsmenuxbox"] = opt.build();
        Movie title(movie(c, "blacktitlemenu")); // QUIT under OPERATOR PROFILE (rows 25.1 apart, revealed 43/46/49)
        main_menu_quit(title, 18, 25.1f, 49, 11, 12, "profileButton", "continueButton",
                       {"continueButton", "optionsButton", "profileButton", "quitButton"});
        c.movies["blacktitlemenu"] = title.build();
        Movie fresh(movie(c, "blacktitlemenu2")); // QUIT under OPTIONS (revealed 43/46)
        main_menu_quit(fresh, 14, 25.0f, 46, 7, 8, "optionsButton", "startButton",
                       {"startButton", "optionsButton", "quitButton"});
        c.movies["blacktitlemenu2"] = fresh.build();
        // the game's own pages the pointer works on (fe/pages.cpp game_page): the same names as the front end's own
        // (input/bindings.cpp pointer_page_up lists them)
        game_pages(c, {"controllersetup", "controllercustomxbox", "soundsetupxbox", "picturesetupxbox", "profileoptions",
                       "operatorprofile", "choosedifficulty", "difficulty", "createblackprofile", "virtualkeyboard",
                       "memcardmessage", "memcardslots", "missionmodeselect", "missionmodeselect2", "unlocks",
                       "unlockweapons", "resultsscreen", "resultseasy"});
    }, fe) && write_file(dir + L"FEMain.bin", fe);
    if (!main_ok) DeleteFileW((dir + L"FEMain.bin").c_str()); // not an old build of the pages
    // the pause menu: PC SETTINGS after OPTIONS (InGameOn.bin) opens the one PC SETTINGS pause page (a copy of the
    // in-game Options in PseMenu.bin, loaded with the pause pages); both files or neither
    Bytes on, pse;
    bool pause_ok = rebuild(game_dir, L"InGameOn.bin", [](Container &c) {
        Movie root(movie(c, "pausemenu"));
        pause_row(root);
        c.movies["pausemenu"] = root.build();
        // confirmwarning is left alone: its own focus handler runs on every SetFocusButton the walk makes (like
        // pausemenuobjectives below), and leaving it focused that way crashed the front end (000fc383) as soon as
        // Back was pressed on it -- a right click on the quit confirmation closed the game.
        game_pages(c, {"objectives"});
    }, on) && rebuild(game_dir, L"PseMenu.bin", [](Container &c) {
        Movie page(movie(c, "pausemenuoptionxbox"));
        pause_page(page);
        add_movie(c, lower(ingame("PCPage")), page.build());
        // pausemenuoptionxbox (the pause menu's OPTIONS) is left alone for the same reason as confirmwarning above:
        // the walk's SetFocusButton runs its own focus handler again, and Back on the patched page crashed the front
        // end at 000fc383 (the user: "no pausa quando eu abro opções com o mouse e clico depois com o click direito, o
        // jogo fecha sozinho"). A click there sends the game's own select, which needs no walk.
    }, pse) && write_file(dir + L"InGameOn.bin", on) && write_file(dir + L"PseMenu.bin", pse);
    if (!pause_ok) { // a half-written pair would point the pause menu at pages that are not there
        DeleteFileW((dir + L"InGameOn.bin").c_str());
        DeleteFileW((dir + L"PseMenu.bin").c_str());
    }
    logf("fe: PC SETTINGS pages: main menu %s, pause menu %s (%s)", main_ok ? "on" : "off", pause_ok ? "on" : "off",
         narrow(out_root).c_str());
    Bytes core;
    bool legend_ok = !g_cfg.input_pad && rebuild(game_dir, L"Core.bin", [](Container &c) { // CONTROLLER: the game's icons
        Movie mv(movie(c, "corexbox"));
        legend_keys(mv);
        c.movies["corexbox"] = mv.build();
    }, core) && write_file(dir + L"Core.bin", core);
    if (!legend_ok) DeleteFileW((dir + L"Core.bin").c_str());
    logf("fe: menu button legend %s", legend_ok ? "shows keys" : "unchanged");
    bool text_ok = patch_strings(game_dir, out_root);
    return main_ok || pause_ok || text_ok;
}
