#include "game/character/CharacterCatalog.h"
#include <cstdio>
int main(int,char**argv){Eagle::Character::CharacterCatalog c;std::string e;if(!c.Load(argv[1],e)){fprintf(stderr,"%s\n",e.c_str());return 1;}fputs(c.UiJson().c_str(),stdout);fputs("\n",stdout);
 fputs(Eagle::Character::AppearanceJson(Eagle::Character::PlayerAppearance{}).c_str(),stdout);}
