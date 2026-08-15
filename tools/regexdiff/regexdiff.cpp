// Differential check: pcre_exec(...) >= 0  vs  std::regex_search (ECMAScript).
//
// Evidence for HARNESS.md §9.2, which replaced PCRE with std::regex. The patterns
// this game matches are *data* -- 153 in Sound/SoundEffects.cfg plus the default
// AutoPickUpMatching -- so swapping the engine swaps the grammar under a config
// file nobody re-tests. This compiles every shipped pattern under both engines and
// compares their verdicts on every distinct string the committed corpora draw.
//
// Patterns: shipped Sound/SoundEffects.cfg + the default AutoPickUpMatching.
// Subjects: every distinct string in the committed corpora text logs.
//
// Deliberately NOT wired into CMake: it needs libpcre, which is the dependency
// §9.2 removed from the build. Same spirit as savediff being EXCLUDE_FROM_ALL --
// an oracle must not be able to break the thing it judges.
//
//   sudo apt-get install -y libpcre3-dev
//   g++ -std=c++11 -O1 -o regexdiff tools/regexdiff/regexdiff.cpp -lpcre
//   ./regexdiff Sound/SoundEffects.cfg tools/corpora/*.text.log
//
// Exit 0 = the two engines agree everywhere. Expected: 0 mismatches.
#include <pcre.h>
#include <regex>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <iostream>

static std::vector<std::string> LoadPatterns(const char* Path)
{
  std::vector<std::string> Out;
  std::ifstream In(Path);
  std::string Line;
  while(std::getline(In, Line))
  {
    if(Line.empty() || Line[0] == '#') continue;
    // field 3 of a ';'-separated line, matching sfx.cpp's hand parser
    size_t A = Line.find(';');
    if(A == std::string::npos) continue;
    size_t B = Line.find(';', A + 1);
    if(B == std::string::npos) continue;
    size_t C = Line.find(';', B + 1);
    std::string P = Line.substr(B + 1, C == std::string::npos ? std::string::npos : C - B - 1);
    if(!P.empty()) Out.push_back(P);
  }
  return Out;
}

static void LoadSubjects(const char* Path, std::set<std::string>& Out)
{
  std::ifstream In(Path);
  std::string Line;
  while(std::getline(In, Line))
  {
    if(Line.size() < 2 || Line[0] != 'T') continue;
    // T <frame> <db|buf> <x> <y> <text>
    size_t P = 0;
    for(int i = 0; i < 5; ++i)
    {
      P = Line.find(' ', P);
      if(P == std::string::npos) break;
      ++P;
    }
    if(P != std::string::npos && P <= Line.size()) Out.insert(Line.substr(P));
  }
}

int main(int argc, char** argv)
{
  std::vector<std::string> Patterns = LoadPatterns(argv[1]);
  // Mirrors the default value of ivanconfig::AutoPickUpMatching (Main/Source/iconf.cpp:56).
  // Keep in sync by hand -- if that default changes, re-run this.
  Patterns.push_back("!((book|can|dagger|grenade|horn of|kiwi|key|ring|scroll|wand|whistle)"
                     "|^(?:(?!(broken|empty)).)*(bottle|vial)|sol stone)");

  std::set<std::string> SubjectSet;
  for(int i = 2; i < argc; ++i) LoadSubjects(argv[i], SubjectSet);
  // a few item-name shapes the corpora may not draw, for the auto-pickup pattern
  const char* Extra[] = { "a dagger", "a broken bottle", "an empty vial", "a bottle",
                          "the sol stone", "a scroll of wishing", "a kiwi", "an empty bottle",
                          "a bottle of water", "a horn of fear", "a broken vial", "" };
  for(unsigned i = 0; i < sizeof(Extra)/sizeof(*Extra); ++i) SubjectSet.insert(Extra[i]);
  std::vector<std::string> Subjects(SubjectSet.begin(), SubjectSet.end());

  int CompileFail = 0, Mismatch = 0;
  long Compared = 0;

  for(size_t i = 0; i < Patterns.size(); ++i)
  {
    const std::string& Pat = Patterns[i];

    const char* Err; int ErrOff;
    pcre* Re = pcre_compile(Pat.c_str(), 0, &Err, &ErrOff, NULL);

    std::regex Std;
    bool StdOk = true;
    try { Std.assign(Pat, std::regex::ECMAScript); }
    catch(const std::regex_error& E)
    {
      StdOk = false;
      std::cout << "COMPILE-FAIL std::regex: '" << Pat << "' -> " << E.what() << "\n";
      ++CompileFail;
    }

    if(!Re)
    {
      std::cout << "(pcre also rejects: '" << Pat << "')\n";
      continue;
    }
    if(!StdOk) { pcre_free(Re); continue; }

    for(size_t j = 0; j < Subjects.size(); ++j)
    {
      const std::string& S = Subjects[j];
      bool A = pcre_exec(Re, NULL, S.c_str(), (int)S.size(), 0, 0, NULL, 0) >= 0;
      bool B = std::regex_search(S.c_str(), S.c_str() + S.size(), Std);
      ++Compared;
      if(A != B)
      {
        ++Mismatch;
        if(Mismatch <= 20)
          std::cout << "MISMATCH pcre=" << A << " std=" << B
                    << "  pat='" << Pat << "'  subj='" << S << "'\n";
      }
    }
    pcre_free(Re);
  }

  std::cout << "\npatterns=" << Patterns.size()
            << " subjects=" << Subjects.size()
            << " comparisons=" << Compared
            << " compile-failures=" << CompileFail
            << " mismatches=" << Mismatch << "\n";
  return (CompileFail || Mismatch) ? 1 : 0;
}
