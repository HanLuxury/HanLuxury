#include "VcConfig.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace vc {
namespace {

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool ParseNumber(std::string_view text, double& out) {
    const std::string copy(text);
    if (copy.empty()) return false;
    char* end = nullptr;
    errno = 0;
    out = strtod(copy.c_str(), &end);
    return errno == 0 && end && *end == '\0' && std::isfinite(out);
}

bool ParseBool(std::string_view text, bool& out) {
    const std::string v = Lower(text);
    if (v == "1" || v == "true" || v == "yes" || v == "on" || v == "ya") {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off" || v == "tidak") {
        out = false;
        return true;
    }
    return false;
}

}  // namespace

const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::Auto: return "auto";
        case Mode::Always: return "always";
        case Mode::Server: return "server";
        case Mode::Off: return "off";
    }
    return "?";
}

void ParseConfig(std::string_view text, Config& out, std::vector<std::string>& notes) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    bool inSection = false;
    int lineNumber = 0;
    while (!text.empty()) {
        const size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
        ++lineNumber;

        const size_t comment = line.find_first_of(";#");
        if (comment != std::string_view::npos) line = line.substr(0, comment);
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            const size_t close = line.find(']');
            inSection = close != std::string_view::npos && Lower(Trim(line.substr(1, close - 1))) == "vicecity";
            continue;
        }
        if (!inSection) continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            notes.push_back("baris " + std::to_string(lineNumber) + ": tidak ada tanda '='");
            continue;
        }
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string_view value = Trim(line.substr(eq + 1));
        const auto bad = [&] {
            notes.push_back("baris " + std::to_string(lineNumber) + ": nilai " + key + " tidak dipahami (\"" +
                            std::string(value) + "\"), dipakai nilai bawaan");
        };
        const auto number = [&](double low, double high, double& result) {
            double v = 0;
            if (!ParseNumber(value, v)) {
                bad();
                return false;
            }
            if (v < low || v > high) {
                notes.push_back("baris " + std::to_string(lineNumber) + ": " + key + " di luar batas, dibatasi ke " +
                                std::to_string(v < low ? low : high));
                v = v < low ? low : high;
            }
            result = v;
            return true;
        };

        double v = 0;
        if (key == "mode") {
            const std::string mode = Lower(value);
            if (mode == "auto") out.mode = Mode::Auto;
            else if (mode == "always" || mode == "selalu") out.mode = Mode::Always;
            else if (mode == "server") out.mode = Mode::Server;
            else if (mode == "off" || mode == "mati") out.mode = Mode::Off;
            else bad();
        } else if (key == "drawdistance") {
            if (number(0.3, 2.0, v)) out.drawDistance = static_cast<float>(v);
        } else if (key == "maxobjects") {
            if (number(200, 8000, v)) out.maxObjects = static_cast<int>(v);
        } else if (key == "budgetms") {
            if (number(1, 50, v)) out.budgetMs = static_cast<int>(v);
        } else if (key == "txdidleseconds") {
            if (number(5, 3600, v)) out.txdIdleSeconds = static_cast<int>(v);
        } else if (key == "firstmodelid") {
            if (number(0, 1000000, v)) out.firstModelId = static_cast<int>(v);
        } else if (key == "specialflags") {
            if (!ParseBool(value, out.specialFlags)) bad();
        } else if (key == "worldpatch") {
            if (!ParseBool(value, out.worldPatch)) bad();
        } else {
            notes.push_back("baris " + std::to_string(lineNumber) + ": pengaturan \"" + key + "\" tidak dikenal");
        }
    }
}

std::string DefaultConfigText() {
    return "; Pengaturan map Vice City. Perubahan terbaca setelah game ditutup lalu dibuka lagi.\n"
           "[ViceCity]\n"
           "; auto   = map dipasang client setelah server memakai model Vice City (filterscript vice_city_037)\n"
           "; always = map dipasang client di server mana pun\n"
           "; server = client tidak memasang apa-apa, model hanya disediakan untuk objek buatan server\n"
           "; off    = map dimatikan\n"
           "Mode = auto\n"
           "\n"
           "; Pengali jarak tampil (0.3 - 2.0). Turunkan kalau HP terasa berat atau kehabisan memori.\n"
           "DrawDistance = 1.0\n"
           "\n"
           "; Jumlah objek map yang boleh ada sekaligus (200 - 8000). Titik terpadat butuh sekitar 1650.\n"
           "MaxObjects = 2500\n"
           "\n"
           "; Waktu per frame (milidetik) untuk membuat objek yang belum mendesak (1 - 50).\n"
           "BudgetMs = 4\n"
           "\n"
           "; File tekstur yang tidak dipakai dilepas dari memori setelah sekian detik (5 - 3600).\n"
           "TxdIdleSeconds = 20\n"
           "\n"
           "; 0 = pakai ID model kosong paling atas. Isi angka lain kalau ID itu bentrok dengan mod lain.\n"
           "FirstModelId = 0\n"
           "\n"
           "; 1 = kaca bisa pecah dan flag khusus lain seperti di script PC. 0 = hanya flag tampilan.\n"
           "SpecialFlags = 0\n";
}

}  // namespace vc
