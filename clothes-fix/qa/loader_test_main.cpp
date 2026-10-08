// Appended after ClothesLoader.cpp's validator and ShortenFrameNames (cut out by run.sh): runs both on DFFs.
#include <fstream>
#include <iostream>
#include <iterator>
using namespace Eagle::Character;
int main(int argc,char**argv) {
    int bad=0,shortened=0;
    for(int i=1;i<argc;++i) {
        std::ifstream f(argv[i],std::ios::binary);std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),{});std::string e;
        if(!ClothesLoader::ValidateDff(b,e)) {++bad;std::cout<<"INVALID "<<argv[i]<<": "<<e<<"\n";continue;}
        std::vector<uint8_t> c(b);std::vector<std::string> names;
        if(!ClothesLoader::ShortenFrameNames(c,names)||c.size()!=b.size()) {++bad;std::cout<<"SHORTEN FAIL "<<argv[i]<<"\n";continue;}
        if(c!=b) ++shortened;
    }
    std::cout<<"DFF checked "<<argc-1<<", rejected "<<bad<<", long frame names rewritten in "<<shortened<<"\n";
    return bad!=0;
}
