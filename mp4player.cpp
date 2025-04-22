#include <vector>
#include <wels/codec_api.h>
#include <SDL2/SDL.h>

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

static uint8_t *preload(const char *path, ssize_t *data_size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    *data_size = 0;
    if (!file)
        return 0;
    if (fseek(file, 0, SEEK_END))
        exit(1);
    *data_size = (ssize_t)ftell(file);
    if (*data_size < 0)
        exit(1);
    if (fseek(file, 0, SEEK_SET))
        exit(1);
    data = (unsigned char*)malloc(*data_size);
    if (!data)
        exit(1);
    if ((ssize_t)fread(data, 1, *data_size, file) != *data_size)
        exit(1);
    fclose(file);
    return data;
}

typedef struct
{
    uint8_t *buffer;
    ssize_t size;
} INPUT_BUFFER;

static int read_callback(int64_t offset, void *buffer, size_t size, void *token)
{
    INPUT_BUFFER *buf = (INPUT_BUFFER*)token;
    size_t to_copy = MINIMP4_MIN(size, buf->size - offset - size);
    memcpy(buffer, buf->buffer + offset, to_copy);
    return to_copy != size;
}

typedef struct
{
    uint8_t *data;
    ssize_t size;
} OUTPUT_BUFFER;

OUTPUT_BUFFER* create_output_buffer(ssize_t initial_size)
{
    OUTPUT_BUFFER *buf = (OUTPUT_BUFFER*)malloc(sizeof(OUTPUT_BUFFER));
    buf->data = (uint8_t*)malloc(initial_size);
    buf->size = 0;
    return buf;
}

void append_to_output_buffer(OUTPUT_BUFFER *buf, const void *data, ssize_t size)
{
    buf->data = (uint8_t*)realloc(buf->data, buf->size + size);
    memcpy(buf->data + buf->size, data, size);
    buf->size += size;
}

void free_output_buffer(OUTPUT_BUFFER *buf)
{
    if (buf)
    {
        if (buf->data)
            free(buf->data);
        free(buf);
    }
}

int demux(uint8_t *input_buf, ssize_t input_size, OUTPUT_BUFFER *output_buf, int ntrack)
{
    int i, spspps_bytes;
    const void *spspps;
    INPUT_BUFFER buf = { input_buf, input_size };
    MP4D_demux_t mp4 = { 0, };
    MP4D_open(&mp4, read_callback, &buf, input_size);

    {
        MP4D_track_t *tr = mp4.track + ntrack;
        unsigned sum_duration = 0;
        i = 0;
        if (tr->handler_type == MP4D_HANDLER_TYPE_VIDE)
        {   // assume h264
#define USE_SHORT_SYNC 0
            char sync[4] = { 0, 0, 0, 1 };
            while (spspps = MP4D_read_sps(&mp4, ntrack, i, &spspps_bytes))
            {
                append_to_output_buffer(output_buf, sync + USE_SHORT_SYNC, 4 - USE_SHORT_SYNC);
                append_to_output_buffer(output_buf, spspps, spspps_bytes);
                i++;
            }
            i = 0;
            while (spspps = MP4D_read_pps(&mp4, ntrack, i, &spspps_bytes))
            {
                append_to_output_buffer(output_buf, sync + USE_SHORT_SYNC, 4 - USE_SHORT_SYNC);
                append_to_output_buffer(output_buf, spspps, spspps_bytes);
                i++;
            }
            for (i = 0; i < mp4.track[ntrack].sample_count; i++)
            {
                unsigned frame_bytes, timestamp, duration;
                MP4D_file_offset_t ofs = MP4D_frame_offset(&mp4, ntrack, i, &frame_bytes, &timestamp, &duration);
                uint8_t *mem = input_buf + ofs;
                sum_duration += duration;
                while (frame_bytes)
                {
                    uint32_t size = ((uint32_t)mem[0] << 24) | ((uint32_t)mem[1] << 16) 
                        | ((uint32_t)mem[2] << 8) | mem[3];
                    size += 4;
                    mem[0] = 0; mem[1] = 0; mem[2] = 0; mem[3] = 1;
                    append_to_output_buffer(output_buf, mem + USE_SHORT_SYNC, size - USE_SHORT_SYNC);
                    if (frame_bytes < size)
                    {
                        printf("error: demux sample failed\n");
                        exit(1);
                    }
                    frame_bytes -= size;
                    mem += size;
                }
            }
        } else if (tr->handler_type == MP4D_HANDLER_TYPE_SOUN)
        { 
            for (i = 0; i < mp4.track[ntrack].sample_count; i++)
            {
                unsigned frame_bytes, timestamp, duration;
                MP4D_file_offset_t ofs = MP4D_frame_offset(&mp4, ntrack, i, &frame_bytes, &timestamp, &duration);
                printf("ofs=%d frame_bytes=%d timestamp=%d duration=%d\n", (unsigned)ofs, frame_bytes, timestamp, duration);
            }
        }
    }
    MP4D_close(&mp4);
    if (input_buf)
        free(input_buf);
    return 0;
}

struct YUVFrame 
{
    uint8_t *data[3];
    int width;
    int height;
};

std::vector<YUVFrame> yuvFrames;

size_t get_nalu_size(uint8_t *data, size_t size, size_t offset) 
{
    size_t pos = offset + 3; // SKip first 3 bytes of start code
    if (data[offset] == 0x00 && data[offset + 1] == 0x00) 
    {
        pos += (data[offset + 2] == 0x01) ? 0 : 1; // NALU 0x000001 / 0x00000001
    }

    while (pos < size - 3) {
        if (data[pos] == 0x00 && data[pos + 1] == 0x00 &&
            (data[pos + 2] == 0x01 || (data[pos + 2] == 0x00 && data[pos + 3] == 0x01))) 
        {
            return pos - offset; // Return NALU size
        }
        pos++;
    }
    return size - offset;
}

void store_yuv_frame(SBufferInfo &bufferInfo, uint8_t *pData[3]) 
{
    YUVFrame frame;
    frame.width = bufferInfo.UsrData.sSystemBuffer.iWidth;
    frame.height = bufferInfo.UsrData.sSystemBuffer.iHeight;
    int strideY = bufferInfo.UsrData.sSystemBuffer.iStride[0];
    int strideUV = bufferInfo.UsrData.sSystemBuffer.iStride[1];
    
    //Reserve memory
    frame.data[0] = (uint8_t *)malloc(frame.width * frame.height);
    frame.data[1] = (uint8_t *)malloc((frame.width / 2) * (frame.height / 2));
    frame.data[2] = (uint8_t *)malloc((frame.width / 2) * (frame.height / 2));
    
    // Copy the planes with stride correction
    for (int y = 0; y < frame.height; y++) 
    {
        memcpy(frame.data[0] + y * frame.width, pData[0] + y * strideY, frame.width);
    }

    for (int y = 0; y < frame.height / 2; y++) 
    {
        memcpy(frame.data[1] + y * (frame.width / 2), pData[1] + y * strideUV, frame.width / 2);
        memcpy(frame.data[2] + y * (frame.width / 2), pData[2] + y * strideUV, frame.width / 2);
    }
    
    // store frame to vector
    yuvFrames.push_back(std::move(frame));
}

void decode_h264_stream(uint8_t *data, ssize_t size)
{
    // Initialize decoder
    ISVCDecoder *decoder;
    WelsCreateDecoder(&decoder);
    SDecodingParam decParam = {0};
    decParam.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    decoder->Initialize(&decParam);
    
    uint8_t *pData[3] = {NULL, NULL, NULL};
    SBufferInfo bufferInfo;
    memset(&bufferInfo, 0, sizeof(SBufferInfo));  
    
    // Decoding process
    ssize_t frame_counter = 0;
    ssize_t offset = 0;
    while (offset < size)
    {
        uint8_t* nalStart = data + offset;
        size_t nalSize = get_nalu_size(data, size, offset);
        
        int iRet = decoder->DecodeFrameNoDelay(nalStart, nalSize, pData, &bufferInfo);
        if (iRet != 0)
            printf("error decoding frame\n");

         if (bufferInfo.iBufferStatus == 1)
        {
            printf("frame decoded ~ NALU size:%d\n", nalSize);
            frame_counter++;
            store_yuv_frame(bufferInfo, pData);              
        } 
        
        // Advance to next NAL Unit
        offset += nalSize;
    }
    printf("%d frames decoded\n", frame_counter);

    decoder->Uninitialize();
    WelsDestroyDecoder(decoder);  
}

void draw_frames()
{
    if (yuvFrames.empty()) 
        return;
    
    int frameIndex = 0;
    
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *window = SDL_CreateWindow("SDL2 YUV Display", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                                          yuvFrames[0].width, yuvFrames[0].height, SDL_WINDOW_SHOWN);
    if (!window) 
    {
        printf("Error creating SDL2 Window: %s\n", SDL_GetError());
        return;
    }
    
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, 
                                             yuvFrames[0].width, yuvFrames[0].height);

    bool running = true;
    SDL_Event event;

    const int frameDelay = 1000 / 60;  // 60 FPS
    Uint32 frameStart;
    int frameTime;

    while (running)
    {
        frameStart = SDL_GetTicks();

        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT || event.type == SDL_KEYDOWN)
                running = false;
        }

        // Obtain current frame
        YUVFrame &frame = yuvFrames[frameIndex];

        SDL_UpdateYUVTexture(texture, NULL, 
            frame.data[0], frame.width, 
            frame.data[1], frame.width / 2, 
            frame.data[2], frame.width / 2);
                                          
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        // Next frame
        frameIndex++;
        if (frameIndex == yuvFrames.size())
            frameIndex = 0;

        // Frametime
        frameTime = SDL_GetTicks() - frameStart;
        if (frameTime < frameDelay)
            SDL_Delay(frameDelay - frameTime);
    }

    // Free resources
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void free_yuvFrames()
{
    for (auto &frame : yuvFrames) 
    {
        for (int i = 0; i < 3; ++i) 
        {
            if (frame.data[i] != nullptr)
                free(frame.data[i]);
        }
    }
    yuvFrames.clear();
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("Usage: %s <input.mp4>\n", argv[0]);
        return 1;
    }

    ssize_t h264_size;
    uint8_t *alloc_buf = preload(argv[1], &h264_size);
    if (!alloc_buf)
    {
        printf("error: can't open input file\n");
        return 1;
    }
    
    // Demux the mp4 file
    int track = 0;
    OUTPUT_BUFFER *output_buf = create_output_buffer(1024 * 1024); // Initial buffer size of 1MB
    demux(alloc_buf, h264_size, output_buf, track);
    printf("Demuxed data size: %zd\n", output_buf->size);
    
    // Decode
    decode_h264_stream(output_buf->data, output_buf->size);
    printf("Decoding finished\n");

    // Draw
    printf("Starting render...\n");
    draw_frames();

    // Free resources
    printf("Closing program\n");
    free_output_buffer(output_buf);
    free_yuvFrames;

    return 0;
}
