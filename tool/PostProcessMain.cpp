#include "PostProcess.h"

#include <string>

int main(int argc, char* argv[]) {

    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " [temp_dir] [output_dir]" << std::endl;
        return 1;
    }
    temp_dir = (argc > 1) ? argv[1] : temp_dir;
    output_dir = (argc > 2) ? argv[2] : output_dir;
    PostProcess<2> post_process(4);
    post_process.merge_buckets();
    post_process.sort_low_kmers();
}