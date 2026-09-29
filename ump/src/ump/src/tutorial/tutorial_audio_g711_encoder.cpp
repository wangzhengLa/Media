#include "audio_coding/g711_encoder.h"
#include <cstdio>
#include <vector>
#include <string>
#include <cstring>
#include <getopt.h>

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options] <input> <output>\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d, --decode      Decode mode (default: encode)\n");
    fprintf(stderr, "  -t, --type type   Codec type: alaw or ulaw (default: alaw)\n");
    fprintf(stderr, "  -h, --help        Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Encode:  input=16-bit PCM file (.pcm), output=G.711 file (.alaw/.ulaw)\n");
    fprintf(stderr, "Decode:  input=G.711 file (.alaw/.ulaw), output=16-bit PCM file (.pcm)\n");
}

int main(int argc, char* argv[]) {
    bool decode_mode = false;
    G711Encoder::Type type = G711Encoder::ALAW;

    // 简单解析命令行参数
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--decode") == 0) {
            decode_mode = true;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--type") == 0) {
            if (i + 1 < argc) {
                ++i;
                if (strcmp(argv[i], "ulaw") == 0)
                    type = G711Encoder::ULAW;
                else if (strcmp(argv[i], "alaw") == 0)
                    type = G711Encoder::ALAW;
                else {
                    fprintf(stderr, "Invalid type: %s\n", argv[i]);
                    print_usage(argv[0]);
                    return 1;
                }
            } else {
                fprintf(stderr, "Option -t requires an argument.\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    // 检查是否提供了输入输出文件
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    // 定位输入输出文件名（最后一个参数是输出，倒数第二个是输入）
    const char* input_file = argv[argc - 2];
    const char* output_file = argv[argc - 1];

    // 打开输入文件
    FILE* fin = fopen(input_file, "rb");
    if (!fin) {
        perror("fopen input");
        return 1;
    }
    fseek(fin, 0, SEEK_END);
    long file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    // 根据模式处理
    G711Encoder encoder(type);

    if (!decode_mode) {
        // 编码模式：输入必须是 PCM（16-bit 小端）
        if (file_size <= 0 || (file_size % sizeof(int16_t)) != 0) {
            fprintf(stderr, "Invalid PCM file size (not multiple of 2 bytes)\n");
            fclose(fin);
            return 1;
        }
        size_t num_samples = file_size / sizeof(int16_t);
        std::vector<int16_t> pcm(num_samples);
        fread(pcm.data(), sizeof(int16_t), num_samples, fin);
        fclose(fin);

        std::vector<uint8_t> encoded = encoder.Encode(pcm.data(), num_samples);

        FILE* fout = fopen(output_file, "wb");
        if (!fout) { perror("fopen output"); return 1; }
        fwrite(encoded.data(), 1, encoded.size(), fout);
        fclose(fout);

        printf("Encoded %zu samples (%zu bytes PCM) to %zu bytes G.711\n",
               num_samples, num_samples * sizeof(int16_t), encoded.size());
    } else {
        // 解码模式：输入是 G.711 8-bit 数据
        if (file_size <= 0) {
            fprintf(stderr, "Empty input file.\n");
            fclose(fin);
            return 1;
        }
        size_t num_bytes = file_size;
        std::vector<uint8_t> g711_data(num_bytes);
        fread(g711_data.data(), 1, num_bytes, fin);
        fclose(fin);

        std::vector<int16_t> pcm = encoder.Decode(g711_data.data(), num_bytes);

        FILE* fout = fopen(output_file, "wb");
        if (!fout) { perror("fopen output"); return 1; }
        fwrite(pcm.data(), sizeof(int16_t), pcm.size(), fout);
        fclose(fout);

        printf("Decoded %zu bytes G.711 to %zu samples (%zu bytes PCM)\n",
               num_bytes, pcm.size(), pcm.size() * sizeof(int16_t));
    }

    return 0;
}
