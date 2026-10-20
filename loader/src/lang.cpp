// The port's own texts in Brazilian Portuguese (lang.h). The game's font has no glyph for ã / õ: the Portuguese
// translation of the game writes ä / ö, which the font draws as ã / õ ("NÄO", "mäo"), so the texts here are written
// properly and mapped the same way when they are shown.
#include "lang.h"

#include "common.h"

#include <cstring>
#include <map>
#include <string>

namespace lang {
namespace {

struct Pair {
    const char *en, *pt;
};

// whole texts: captions, page titles, values
const Pair kTexts[] = {
    // pages
    {"PC SETTINGS", "CONFIGURAÇÕES DO PC"},
    {"VIDEO", "VÍDEO"},
    {"DISPLAY", "TELA"},
    {"AUDIO AND GAME", "ÁUDIO E JOGO"},
    {"CONTROLS", "CONTROLES"},
    {"MODS", "MODS"},
    {"KEY BINDINGS", "TECLAS"},
    {"QUIT", "SAIR"},
    // rows
    {"RESOLUTION", "RESOLUÇÃO"},
    {"ANTI-ALIASING", "ANTISSERRILHADO"},
    {"FIELD OF VIEW", "CAMPO DE VISÃO"},
    {"BRIGHTNESS", "BRILHO"},
    {"GAMMA", "GAMA"},
    {"FULLSCREEN", "TELA CHEIA"},
    {"ASPECT RATIO", "PROPORÇÃO DA TELA"},
    {"FRAME RATE", "TAXA DE QUADROS"},
    {"FILL GEOMETRY SEAMS", "PREENCHER FRESTAS"},
    {"EFFECTS AT XBOX SIZE", "EFEITOS NO TAMANHO DO XBOX"},
    {"ROOM REVERB", "REVERBERAÇÃO"},
    {"AUTO-AIM", "MIRA AUTOMÁTICA"},
    {"SKIP ANY MOVIE", "PULAR QUALQUER VÍDEO"},
    {"MOUSE SENSITIVITY", "SENSIBILIDADE DO MOUSE"},
    {"INVERT MOUSE", "INVERTER MOUSE"},
    {"AIM", "MIRAR"},
    {"CROUCH", "AGACHAR"},
    {"WEAPON FOV", "FOV DA ARMA"},
    {"FPS COUNTER", "CONTADOR DE FPS"},
    {"MOVIE SUBTITLES", "LEGENDAS DOS VÍDEOS"},
    {"RESOLUTION SCALE", "ESCALA DE RESOLUÇÃO"},
    {"ANISOTROPIC FILTERING", "FILTRO ANISOTRÓPICO"},
    {"WINDOW SIZE", "TAMANHO DA JANELA"},
    {"CAMERA SHAKE", "TREMOR DA CÂMERA"},
    {"RELOAD BLUR", "DESFOQUE NA RECARGA"},
    {"TEXTURES", "TEXTURAS"},
    {"LANGUAGE", "IDIOMA"},
    {"DRAW DISTANCE", "DISTÂNCIA DE VISÃO"},
    {"INPUT", "ENTRADA"},
    {"INPUT DEVICE", "DISPOSITIVO"},
    {"MOUSE/KEYBOARD", "MOUSE/TECLADO"},
    {"CONTROLLER", "CONTROLE"},
    // values
    {"ON", "LIGADO"},
    {"OFF", "DESLIGADO"},
    {"CUSTOM", "PERSONALIZADO"},
    {"1.5 X WINDOW", "1.5 X JANELA"},
    {"2 X WINDOW", "2 X JANELA"},
    {"CONTROLLER ONLY", "SÓ NO CONTROLE"},
    {"HOLD", "SEGURAR"},
    {"TOGGLE", "ALTERNAR"},
    {"PROFILE OPTION", "OPÇÃO DO PERFIL"},
    // key bindings
    {"RESET TO DEFAULTS", "RESTAURAR PADRÕES"},
    {"PRESS A KEY", "APERTE UMA TECLA"},
    {"WHEEL UP", "RODA P/ CIMA"},
    {"WHEEL DOWN", "RODA P/ BAIXO"},
    {"RUN FORWARD", "CORRER P/ FRENTE"},
    {"RUN BACK", "CORRER P/ TRÁS"},
    {"SIDESTEP LEFT", "PASSO P/ ESQUERDA"},
    {"SIDESTEP RIGHT", "PASSO P/ DIREITA"},
    {"FIRE", "ATIRAR"},
    {"RELOAD", "RECARREGAR"},
    {"USE, PICK UP", "USAR, PEGAR"},
    {"GRENADE", "GRANADA"},
    {"MELEE", "CORPO A CORPO"},
    {"NEXT WEAPON", "PRÓXIMA ARMA"},
    {"PREVIOUS WEAPON", "ARMA ANTERIOR"},
    {"CHANGE FIRING MODE", "MODO DE DISPARO"},
    {"TOGGLE SUPPRESSOR", "SILENCIADOR"},
    {"HEALTH PACK", "KIT MÉDICO"},
    {"NEXT MUSIC TRACK", "PRÓXIMA MÚSICA"},
    {"TURN LEFT", "VIRAR P/ ESQUERDA"},
    {"TURN RIGHT", "VIRAR P/ DIREITA"},
    {"LOOK UP", "OLHAR P/ CIMA"},
    {"LOOK DOWN", "OLHAR P/ BAIXO"},
    {"PAUSE", "PAUSAR"},
    {"SHOW OBJECTIVES", "VER OBJETIVOS"},
    {"WEAPON 1", "ARMA 1"},
    {"WEAPON 2", "ARMA 2"},
    // the menus' button legend
    {"[ARROWS]", "[SETAS]"},
};

// the endings a value can carry, translated on their own
const Pair kSuffixes[] = {
    {" (NEXT START)", " (AO REINICIAR)"},
    {" (RESTART)", " (REINICIA)"},
    {" (LANGUAGE)", " (IDIOMA)"},
    {" (ORIGINAL)", " (ORIGINAL)"},
};

// ã õ Ã Õ as the game's font has them (see the top)
std::string font_safe(const char *s)
{
    std::string out = s;
    const Pair map[] = {{"ã", "ä"}, {"õ", "ö"}, {"Ã", "Ä"}, {"Õ", "Ö"}};
    for (const Pair &m : map)
        for (size_t p = out.find(m.en); p != std::string::npos; p = out.find(m.en, p + strlen(m.pt)))
            out.replace(p, strlen(m.en), m.pt);
    return out;
}

const std::map<std::string, std::string> &table()
{
    static std::map<std::string, std::string> t = [] {
        std::map<std::string, std::string> m;
        for (const Pair &p : kTexts) m[p.en] = font_safe(p.pt);
        return m;
    }();
    return t;
}

} // namespace

bool portuguese() { return g_cfg.language == kPortuguese; }

const char *tr(const char *en)
{
    if (!portuguese() || !en) return en;
    auto it = table().find(en);
    return it == table().end() ? en : it->second.c_str();
}

std::wstring tr(const std::wstring &en)
{
    if (!portuguese() || en.empty()) return en;
    std::string s = narrow(en), tail;
    for (bool found = true; found;) { // the endings, last first
        found = false;
        for (const Pair &p : kSuffixes) {
            size_t n = strlen(p.en);
            if (s.size() > n && s.compare(s.size() - n, n, p.en) == 0) {
                tail = font_safe(p.pt) + tail;
                s.resize(s.size() - n);
                found = true;
            }
        }
    }
    auto it = table().find(s);
    return widen((it == table().end() ? s : it->second) + tail);
}

const char *code(int language) { return language == kPortuguese ? "ptbr" : "en"; }

int from_code(const char *c)
{
    if (!_stricmp(c, "ptbr") || !_stricmp(c, "pt-br") || !_stricmp(c, "pt")) return kPortuguese;
    if (!_stricmp(c, "en")) return kEnglish;
    return -1;
}

std::wstring root() { return portuguese() ? g_cfg.port_dir + L"\\mods\\language\\ptbr" : L""; }

} // namespace lang
