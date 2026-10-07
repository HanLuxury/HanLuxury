#include "vctest.h"

#include "../VcConfig.h"

namespace {

struct Parsed {
    vc::Config config;
    std::vector<std::string> notes;
};

Parsed Parse(const std::string& text) {
    Parsed p;
    vc::ParseConfig(text, p.config, p.notes);
    return p;
}

bool HasNote(const Parsed& p, const char* part) {
    for (const std::string& n : p.notes) {
        if (n.find(part) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

ML_TEST(config_defaults) {
    const vc::Config c;
    ML_CHECK(c.mode == vc::Mode::Auto);
    ML_CHECK_EQ(c.drawDistance, 1.0f);
    ML_CHECK_EQ(c.maxObjects, 2500);
    ML_CHECK_EQ(c.budgetMs, 4);
    ML_CHECK_EQ(c.txdIdleSeconds, 20);
    ML_CHECK_EQ(c.firstModelId, 0);
    ML_CHECK(!c.specialFlags);
    ML_CHECK(c.worldPatch);

    const Parsed empty = Parse("");
    ML_CHECK(empty.notes.empty());
    ML_CHECK(empty.config.mode == vc::Mode::Auto);
    ML_CHECK_EQ(empty.config.maxObjects, 2500);
}

// The file the client writes on its first start must read back as the defaults, without a single remark.
ML_TEST(config_default_text_is_the_defaults) {
    const Parsed p = Parse(vc::DefaultConfigText());
    ML_CHECK(p.notes.empty());
    for (const std::string& n : p.notes) printf("  note: %s\n", n.c_str());
    const vc::Config d;
    ML_CHECK(p.config.mode == d.mode);
    ML_CHECK_EQ(p.config.drawDistance, d.drawDistance);
    ML_CHECK_EQ(p.config.maxObjects, d.maxObjects);
    ML_CHECK_EQ(p.config.budgetMs, d.budgetMs);
    ML_CHECK_EQ(p.config.txdIdleSeconds, d.txdIdleSeconds);
    ML_CHECK_EQ(p.config.firstModelId, d.firstModelId);
    ML_CHECK_EQ(p.config.specialFlags, d.specialFlags);
    ML_CHECK_EQ(p.config.worldPatch, d.worldPatch);
    // Every setting that can be given is named in it, so a user finds it without the documentation.
    const std::string text = vc::DefaultConfigText();
    for (const char* key : {"Mode", "DrawDistance", "MaxObjects", "BudgetMs", "TxdIdleSeconds", "FirstModelId", "SpecialFlags"}) {
        ML_CHECK(text.find(std::string(key) + " = ") != std::string::npos);
    }
}

ML_TEST(config_reads_every_setting) {
    const Parsed p = Parse("[ViceCity]\n"
                           "Mode = always\n"
                           "DrawDistance = 0.75\n"
                           "MaxObjects = 1800\n"
                           "BudgetMs = 8\n"
                           "TxdIdleSeconds = 60\n"
                           "FirstModelId = 15000\n"
                           "SpecialFlags = 1\n"
                           "WorldPatch = 0\n");
    ML_CHECK(p.notes.empty());
    ML_CHECK(p.config.mode == vc::Mode::Always);
    ML_CHECK_EQ(p.config.drawDistance, 0.75f);
    ML_CHECK_EQ(p.config.maxObjects, 1800);
    ML_CHECK_EQ(p.config.budgetMs, 8);
    ML_CHECK_EQ(p.config.txdIdleSeconds, 60);
    ML_CHECK_EQ(p.config.firstModelId, 15000);
    ML_CHECK(p.config.specialFlags);
    ML_CHECK(!p.config.worldPatch);
}

ML_TEST(config_modes) {
    ML_CHECK(Parse("[vicecity]\nmode=auto").config.mode == vc::Mode::Auto);
    ML_CHECK(Parse("[vicecity]\nmode=ALWAYS").config.mode == vc::Mode::Always);
    ML_CHECK(Parse("[vicecity]\nmode=selalu").config.mode == vc::Mode::Always);
    ML_CHECK(Parse("[vicecity]\nmode=Server").config.mode == vc::Mode::Server);
    ML_CHECK(Parse("[vicecity]\nmode=off").config.mode == vc::Mode::Off);
    ML_CHECK(Parse("[vicecity]\nmode=mati").config.mode == vc::Mode::Off);

    const Parsed bad = Parse("[vicecity]\nmode=sometimes");
    ML_CHECK(bad.config.mode == vc::Mode::Auto);
    ML_CHECK_EQ(bad.notes.size(), 1u);
    ML_CHECK(HasNote(bad, "baris 2"));
    ML_CHECK(HasNote(bad, "sometimes"));

    ML_CHECK_EQ(std::string(vc::ModeName(vc::Mode::Auto)), "auto");
    ML_CHECK_EQ(std::string(vc::ModeName(vc::Mode::Always)), "always");
    ML_CHECK_EQ(std::string(vc::ModeName(vc::Mode::Server)), "server");
    ML_CHECK_EQ(std::string(vc::ModeName(vc::Mode::Off)), "off");
    // What ModeName prints is accepted back.
    for (vc::Mode m : {vc::Mode::Auto, vc::Mode::Always, vc::Mode::Server, vc::Mode::Off}) {
        ML_CHECK(Parse(std::string("[ViceCity]\nMode = ") + vc::ModeName(m)).config.mode == m);
    }
}

// Files edited on a phone or copied from Windows: BOM, CRLF, tabs, comments behind a value, odd capitals.
ML_TEST(config_tolerates_editor_habits) {
    const Parsed p = Parse("\xEF\xBB\xBF; komentar\r\n"
                           "  [ ViceCity ]  \r\n"
                           "\tMODE\t=\tServer   ; di belakang nilai\r\n"
                           "drawdistance=1.5#komentar\r\n"
                           "\r\n"
                           "  MaxObjects   =   3000  \r\n"
                           "SpecialFlags = Ya\r\n"
                           "WorldPatch = TIDAK\r\n");
    ML_CHECK(p.notes.empty());
    for (const std::string& n : p.notes) printf("  note: %s\n", n.c_str());
    ML_CHECK(p.config.mode == vc::Mode::Server);
    ML_CHECK_EQ(p.config.drawDistance, 1.5f);
    ML_CHECK_EQ(p.config.maxObjects, 3000);
    ML_CHECK(p.config.specialFlags);
    ML_CHECK(!p.config.worldPatch);

    // No newline at the end of the last line.
    ML_CHECK_EQ(Parse("[ViceCity]\nMaxObjects = 900").config.maxObjects, 900);
    // Booleans in every spelling.
    for (const char* yes : {"1", "true", "YES", "On", "ya"}) {
        ML_CHECK(Parse(std::string("[ViceCity]\nSpecialFlags=") + yes).config.specialFlags);
    }
    for (const char* no : {"0", "false", "No", "OFF", "tidak"}) {
        ML_CHECK(!Parse(std::string("[ViceCity]\nWorldPatch=") + no).config.worldPatch);
    }
}

ML_TEST(config_ignores_other_sections) {
    const Parsed p = Parse("MaxObjects = 300\n"             // before any section
                           "[Lain]\n"
                           "MaxObjects = 400\n"
                           "Mode = off\n"
                           "ini bukan pengaturan\n"
                           "[ViceCity]\n"
                           "BudgetMs = 10\n"
                           "[Lagi]\n"
                           "BudgetMs = 20\n"
                           "[VICECITY]\n"                   // the section may come back
                           "TxdIdleSeconds = 100\n"
                           "[ViceCity\n"                    // not closed: not a section of ours
                           "TxdIdleSeconds = 200\n");
    ML_CHECK(p.notes.empty());
    ML_CHECK(p.config.mode == vc::Mode::Auto);
    ML_CHECK_EQ(p.config.maxObjects, 2500);
    ML_CHECK_EQ(p.config.budgetMs, 10);
    ML_CHECK_EQ(p.config.txdIdleSeconds, 100);
}

ML_TEST(config_clamps_and_reports) {
    const Parsed p = Parse("[ViceCity]\n"
                           "DrawDistance = 9\n"
                           "MaxObjects = 5\n"
                           "BudgetMs = 0\n"
                           "TxdIdleSeconds = 999999\n"
                           "FirstModelId = -4\n");
    ML_CHECK_EQ(p.config.drawDistance, 2.0f);
    ML_CHECK_EQ(p.config.maxObjects, 200);
    ML_CHECK_EQ(p.config.budgetMs, 1);
    ML_CHECK_EQ(p.config.txdIdleSeconds, 3600);
    ML_CHECK_EQ(p.config.firstModelId, 0);
    ML_CHECK_EQ(p.notes.size(), 5u);
    ML_CHECK(HasNote(p, "baris 2"));
    ML_CHECK(HasNote(p, "baris 6"));
    ML_CHECK(HasNote(p, "di luar batas"));

    const Parsed low = Parse("[ViceCity]\nDrawDistance = 0.01\nMaxObjects = 100000\n");
    ML_CHECK_EQ(low.config.drawDistance, 0.3f);
    ML_CHECK_EQ(low.config.maxObjects, 8000);

    // The limits themselves are fine.
    const Parsed edge = Parse("[ViceCity]\nDrawDistance = 0.3\nMaxObjects = 8000\nBudgetMs = 50\nTxdIdleSeconds = 5\n");
    ML_CHECK(edge.notes.empty());
    ML_CHECK_EQ(edge.config.drawDistance, 0.3f);
    ML_CHECK_EQ(edge.config.maxObjects, 8000);
    ML_CHECK_EQ(edge.config.budgetMs, 50);
    ML_CHECK_EQ(edge.config.txdIdleSeconds, 5);
}

// Whatever cannot be understood leaves the setting at its default and says which line it was.
ML_TEST(config_rejects_nonsense) {
    const Parsed p = Parse("[ViceCity]\n"
                           "DrawDistance = banyak\n"
                           "MaxObjects = 12abc\n"
                           "BudgetMs =\n"
                           "TxdIdleSeconds = nan\n"
                           "FirstModelId = inf\n"
                           "SpecialFlags = mungkin\n"
                           "WorldPatch = 2\n"
                           "Kecepatan = 3\n"
                           "tanpa tanda sama dengan\n"
                           "MaxObjects = 1e400\n");
    const vc::Config d;
    ML_CHECK_EQ(p.config.drawDistance, d.drawDistance);
    ML_CHECK_EQ(p.config.maxObjects, d.maxObjects);
    ML_CHECK_EQ(p.config.budgetMs, d.budgetMs);
    ML_CHECK_EQ(p.config.txdIdleSeconds, d.txdIdleSeconds);
    ML_CHECK_EQ(p.config.firstModelId, d.firstModelId);
    ML_CHECK_EQ(p.config.specialFlags, d.specialFlags);
    ML_CHECK_EQ(p.config.worldPatch, d.worldPatch);
    ML_CHECK_EQ(p.notes.size(), 10u);
    for (int line = 2; line <= 11; ++line) {
        ML_CHECK(HasNote(p, ("baris " + std::to_string(line) + ":").c_str()));
    }
    ML_CHECK(HasNote(p, "tidak dikenal"));
    ML_CHECK(HasNote(p, "tidak ada tanda '='"));

    // A later valid line still wins over an earlier bad one, and the other way round keeps the good value.
    const Parsed mixed = Parse("[ViceCity]\nMaxObjects = x\nMaxObjects = 700\nBudgetMs = 9\nBudgetMs = y\n");
    ML_CHECK_EQ(mixed.config.maxObjects, 700);
    ML_CHECK_EQ(mixed.config.budgetMs, 9);
    ML_CHECK_EQ(mixed.notes.size(), 2u);
}

// Numbers are cut to whole values for the settings that are whole; huge inputs do nothing bad.
ML_TEST(config_odd_numbers_and_sizes) {
    ML_CHECK_EQ(Parse("[ViceCity]\nMaxObjects = 1234.9\n").config.maxObjects, 1234);
    ML_CHECK_EQ(Parse("[ViceCity]\nMaxObjects = 0x400\n").config.maxObjects, 1024);   // strtod reads hex
    ML_CHECK_EQ(Parse("[ViceCity]\nMaxObjects = +300\n").config.maxObjects, 300);
    ML_CHECK_EQ(Parse("[ViceCity]\nDrawDistance = 1,5\n").config.drawDistance, 1.0f);   // a comma is not a number

    std::string big = "[ViceCity]\n";
    for (int i = 0; i < 20000; ++i) big += "Kunci" + std::to_string(i) + " = " + std::string(40, 'x') + "\n";
    big += "MaxObjects = 4321\n";
    big += std::string(200000, '=') + "\n";
    big += std::string(100000, '[') + "\n";
    const Parsed p = Parse(big);
    ML_CHECK_EQ(p.config.maxObjects, 4321);
    ML_CHECK(p.notes.size() >= 20000);

    // Bytes outside ASCII and embedded NULs.
    std::string odd = "[ViceCity]\nMode = \xFF\xFE\nDrawDistance = 1.25\n";
    odd += std::string("Max\0Objects = 3\n", 16);
    odd += "BudgetMs = 7\n";
    const Parsed q = Parse(odd);
    ML_CHECK(q.config.mode == vc::Mode::Auto);
    ML_CHECK_EQ(q.config.drawDistance, 1.25f);
    ML_CHECK_EQ(q.config.maxObjects, 2500);
    ML_CHECK_EQ(q.config.budgetMs, 7);
}
