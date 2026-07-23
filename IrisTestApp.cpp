/*
 **************************************************************************************************
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 **************************************************************************************************
*/

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <regex>
#include <chrono>
#include <string>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <unordered_map>

#include "ConfigParser.h"
#include "Log.h"
#include "Trace.h"
#include "V4l2Decoder.h"
#include "V4l2Driver.h"
#include "V4l2Encoder.h"

#define SUCCESS 0

#define TEST_APP_VERSION "1.15"

uint32_t gLogLevel = 0xF;

std::unordered_map<std::string, unsigned int> gCodecIDMap = {
    {"VP9", V4L2_PIX_FMT_VP9},
    {"AV1", V4L2_PIX_FMT_AV1},
    {"AVC", V4L2_PIX_FMT_H264},
    {"HEVC", V4L2_PIX_FMT_HEVC},
};

std::unordered_map<std::string, unsigned int> gColorFormatIDMap = {
    {"NV12", V4L2_PIX_FMT_NV12},
    {"QC08C", V4L2_PIX_FMT_QC08C},
    {"QC10C", V4L2_PIX_FMT_QC10C},
};

void handle(int signal_num, siginfo_t* info, void* context) {
    int saved_errno = errno;
    PrintCrashTrace(signal_num, info, context);

    errno = saved_errno;
    signal(signal_num, SIG_DFL);
    raise(signal_num);
}

void InitSignalHandler() {
    struct sigaction action = {};
    action.sa_sigaction = handle;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER;

    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGTRAP, &action, nullptr);
}

int TestingDecoder(ConfigureStruct& config, std::string sessionId) {
    std::shared_ptr<V4l2Decoder> mDecoder = nullptr;
    std::shared_ptr<V4l2DecoderCB> mDecoderCB = nullptr;
    unsigned int codecFmt, pixelFmt;
    int ret = 0;

    codecFmt = gCodecIDMap[config.CodecName];
    pixelFmt = gColorFormatIDMap[config.PixelFormat];

    mDecoder = std::make_shared<V4l2Decoder>(codecFmt, pixelFmt, sessionId);
    mDecoderCB = std::make_shared<V4l2DecoderCB>(mDecoder.get(), sessionId);

    ret = mDecoder->setMemoryType(config.MemoryType);
    TRACE_RETURN_IF_ERROR(ret, "TestingDecoder: setMemoryType failed");
    ret = mDecoder->init();
    TRACE_RETURN_IF_ERROR(ret, "TestingDecoder: init failed");
    ret = mDecoder->initFFStreamParser(config.InputPath);
    TRACE_RETURN_IF_ERROR(ret, "TestingDecoder: initFFStreamParser failed");
    ret = mDecoder->registerCallbacks(mDecoderCB);
    TRACE_RETURN_IF_ERROR(ret, "TestingDecoder: registerCallbacks failed");

    ret = mDecoder->populateDynamicCommands(config.dynamicCommands);
    TRACE_RETURN_IF_ERROR(ret, "TestingDecoder: populateDynamicCommands failed");

    mDecoder->setDump(config.DumpInputPath, config.Outputpath);

    ret = mDecoder->setInputSizeOverWrite(2 * 1024 * 1024);
    TRACE_RECORD_IF_ERROR(ret, "TestingDecoder: setInputSizeOverWrite failed");
    if (!ret) {
        ret = mDecoder->setInputActualCount(config.InputBufferCount);
        TRACE_RECORD_IF_ERROR(ret, "TestingDecoder: setInputActualCount failed");
    }
    if (!ret) {
        ret = mDecoder->setOutputActualCount(config.OutputBufferCount);
        TRACE_RECORD_IF_ERROR(ret, "TestingDecoder: setOutputActualCount failed");
    }
    if (!ret) {
        ret = mDecoder->setResolution(config.Width, config.Height);
        TRACE_RECORD_IF_ERROR(ret, "TestingDecoder: setResolution failed");
    }
    if (!ret) {
        ret = mDecoder->configureInput();
        TRACE_RECORD_IF_ERROR(ret, "TestingDecoder: configureInput failed");
    }
    if (!ret) {
        ret = mDecoder->allocateBuffers(INPUT_PORT);
        TRACE_RECORD_IF_ERROR(ret, "TestingDecoder: allocate input buffers failed");
    }
    if (!ret) {
        ret = mDecoder->startInput();
        TRACE_RECORD_IF_ERROR(ret, "TestingDecoder: startInput failed");
    }
    if (!ret) {
        ret = mDecoder->queueBuffers(config.NumFrames);
        TRACE_RECORD_IF_ERROR(ret, "TestingDecoder: queueBuffers failed");
    }

    mDecoder->stopOutput();
    mDecoder->stopInput();
    mDecoder->deinitFFStreamParser();
    mDecoder->freeBuffers(OUTPUT_PORT);
    mDecoder->freeBuffers(INPUT_PORT);
    mDecoder->deinit();
    if (!ret) {
        printf("**************\nSUCCESS!\n**************\n");
    } else {
        printf("!!!!!!!!!!!!!!\nFAILED!\n!!!!!!!!!!!!!!\n");
    }
    return ret;
}

int TestingEncoder(ConfigureStruct& config, std::string sessionId) {
    std::shared_ptr<V4l2Encoder> mEncoder = nullptr;
    std::shared_ptr<V4l2EncoderCB> mEncoderCB = nullptr;
    unsigned int codecFmt, pixelFmt;
    int ret = 0;

    codecFmt = gCodecIDMap[config.CodecName];
    pixelFmt = gColorFormatIDMap[config.PixelFormat];

    mEncoder = std::make_shared<V4l2Encoder>(codecFmt, pixelFmt, sessionId);
    mEncoderCB = std::make_shared<V4l2EncoderCB>(mEncoder.get(), sessionId);

    ret = mEncoder->setMemoryType(config.MemoryType);
    TRACE_RETURN_IF_ERROR(ret, "TestingEncoder: setMemoryType failed");
    ret = mEncoder->init();
    TRACE_RETURN_IF_ERROR(ret, "TestingEncoder: init failed");
    ret = mEncoder->initFFYUVParser(config.InputPath, config.Width,
                                    config.Height, config.PixelFormat);
    TRACE_RETURN_IF_ERROR(ret, "TestingEncoder: initFFYUVParser failed");

    ret = mEncoder->registerCallbacks(mEncoderCB);
    TRACE_RETURN_IF_ERROR(ret, "TestingEncoder: registerCallbacks failed");

    ret = mEncoder->populateStaticConfigs(config.staticControls);
    TRACE_RETURN_IF_ERROR(ret, "TestingEncoder: populateStaticConfigs failed");

    ret = mEncoder->populateDynamicConfigs(config.dynamicControls);
    TRACE_RETURN_IF_ERROR(ret, "TestingEncoder: populateDynamicConfigs failed");

    ret = mEncoder->populateDynamicCommands(config.dynamicCommands);
    TRACE_RETURN_IF_ERROR(ret, "TestingEncoder: populateDynamicCommands failed");

    if (config.InputBufferCount > 0) {
        mEncoder->setInputActualCount(config.InputBufferCount);
    }
    if (config.OutputBufferCount > 0) {
        mEncoder->setOutputActualCount(config.OutputBufferCount);
    }
    mEncoder->setResolution(config.Width, config.Height);
    mEncoder->setDownScaleResolution(config.DownScaleWidth, config.DownScaleHeight);
    mEncoder->setNALEncoding(false);
    mEncoder->setDump(config.DumpInputPath, config.Outputpath);

    ret = mEncoder->setOperatingRate(1, config.OperatingRate);
    TRACE_RECORD_IF_ERROR(ret, "TestingEncoder: setOperatingRate failed");
    if (!ret) {
        ret = mEncoder->setFrameRate(1, config.FrameRate);
        TRACE_RECORD_IF_ERROR(ret, "TestingEncoder: setFrameRate failed");
    }
    if (!ret) {
        ret = mEncoder->setStaticControls();
        TRACE_RECORD_IF_ERROR(ret, "TestingEncoder: setStaticControls failed");
    }
    if (!ret) {
        ret = mEncoder->configureInput();
        TRACE_RECORD_IF_ERROR(ret, "TestingEncoder: configureInput failed");
    }
    if (!ret) {
        ret = mEncoder->configureOutput();
        TRACE_RECORD_IF_ERROR(ret, "TestingEncoder: configureOutput failed");
    }
    if (!ret) {
        ret = mEncoder->allocateBuffers(OUTPUT_PORT);
        TRACE_RECORD_IF_ERROR(ret, "TestingEncoder: allocate output buffers failed");
    }
    if (!ret) {
        ret = mEncoder->allocateBuffers(INPUT_PORT);
        TRACE_RECORD_IF_ERROR(ret, "TestingEncoder: allocate input buffers failed");
    }
    if (!ret) {
        ret = mEncoder->startOutput();
        TRACE_RECORD_IF_ERROR(ret, "TestingEncoder: startOutput failed");
    }
    if (!ret) {
        ret = mEncoder->startInput();
        TRACE_RECORD_IF_ERROR(ret, "TestingEncoder: startInput failed");
    }
    if (!ret) {
        ret = mEncoder->queueBuffers(config.NumFrames);
        TRACE_RECORD_IF_ERROR(ret, "TestingEncoder: queueBuffers failed");
    }

    mEncoder->stopInput();
    mEncoder->stopOutput();
    mEncoder->deinitFFYUVParser();
    mEncoder->freeBuffers(INPUT_PORT);
    mEncoder->freeBuffers(OUTPUT_PORT);
    mEncoder->deinit();
    if (!ret) {
        printf("**************\nSUCCESS!\n**************\n");
    } else {
        printf("!!!!!!!!!!!!!!\nFAILED!\n!!!!!!!!!!!!!!\n");
    }
    return ret;
}

int getRegexMatchFileNames(std::string regexPath,
                           std::vector<std::string>& matched_files,
                           std::string& pathToFile) {
    int i;

    if (regexPath.empty()) {
        printf("Error: no configure file found. Run \"./iris_v4l2_test --help\" for more info.\n");
        PrintCurrentTrace("getRegexMatchFileNames: empty config path");
        return -EINVAL;
    }

    for (i = regexPath.size() - 1; i >= 0; i--) {
        if (regexPath[i] == '/') {
            break;
        }
    }

    std::string file_name_regex = regexPath.substr(i + 1);
    std::cout << "File name regex : " << file_name_regex << std::endl;
    std::string directory_path = regexPath.substr(0, i);
    pathToFile = directory_path;
    std::cout << "Directory path : " << directory_path << std::endl;
    std::regex star_replace("\\*");
    std::regex questionmark_replace("\\?");

    auto wildcard_pattern = std::regex_replace(
        std::regex_replace(file_name_regex, star_replace, ".*"), questionmark_replace, ".");

    std::cout << "Wildcard: " << file_name_regex << std::endl
              << "Wildcard Pattern: " << wildcard_pattern << std::endl;

    std::regex wildcard_regex(wildcard_pattern);

    for (const auto& entry : std::filesystem::directory_iterator(directory_path)) {
        if (entry.is_regular_file() &&
            std::regex_match(entry.path().filename().string(), wildcard_regex)) {
            matched_files.push_back(entry.path().filename().string());
        }
    }

    std::cout << "Matched files:\n";
    for (const auto& filename : matched_files) {
        std::cout << filename << '\n';
    }

    return 0;
}

void RunSingleTest(std::string test,
                   std::unordered_map<std::string, ConfigureStruct>& mapTestCasesConfig,
                   std::ofstream& resultFile) {
    int ret = 0;
    auto& config = mapTestCasesConfig[test];
    ResetFailureTraceFlag();

    if (config.Domain.compare("Decoder") == 0) {
        ret = TestingDecoder(config, test);
    } else {
        ret = TestingEncoder(config, test);
    }

    if (ret) {
        if (!IsFailureTracePrinted()) {
            PrintCurrentTrace("testcase returned failure");
        }
        std::cout << "Testcase[ " << test << "] : Failed" << std::endl;
        resultFile << "Testcase[ " << test << "] : Failed" << std::endl;
    } else {
        std::cout << "Testcase[ " << test << "] : Passed" << std::endl;
        resultFile << "Testcase[ " << test << "] : Passed" << std::endl;
    }
}

void runAndWaitForComplete(
        std::string& ExecutionMode, std::unordered_map<std::string,
        ConfigureStruct>& mapTestCasesConfig, std::ofstream & resultFile) {
    auto waitFunc = [&](std::string test) -> void {
        return RunSingleTest(test, mapTestCasesConfig, resultFile);
    };

    std::vector<std::shared_ptr<std::thread>> threads;
    if (ExecutionMode == "Concurrent") {
        threads.reserve(mapTestCasesConfig.size());
    }

    for (auto [test, config] : mapTestCasesConfig) {
        if (ExecutionMode == "Concurrent") {
            threads.push_back(std::make_shared<std::thread>(waitFunc, test));
        } else {
            RunSingleTest(test, mapTestCasesConfig, resultFile);
        }
    }

    if (ExecutionMode == "Concurrent") {
        for (auto thread : threads) {
            if (thread->joinable()) {
                thread->join();
            }
        }
    }

    return;
}

void showUsage() {
    printf("iris_v4l2_test [V4L Video Test app] \n");
    printf("Usage : iris_v4l2_test [OPTIONS] CONFIG.json\n");
    printf("[OPTIONS] : --help       : No Argument Required         : Display the options\n");
    printf("[OPTIONS] : --config     : Argument Required            : Absolute path of config file\n");
    printf("[OPTIONS] : --results    : Optional Argument Required   : Absolute path of Results.csv\n");
    printf("[OPTIONS] : --loglevel   : Optional Argument Required   : Absolute path of config file\n");
}

int main(int argc, char** argv) {
    int ret, option, codec = 0;
    std::string configPath = "", resultsPath = "";

    InitSignalHandler();

    std::ofstream resultFile;
    if (resultsPath == "") {
        resultsPath = "/etc/Results.csv";
    }

    resultFile.open(resultsPath, std::ofstream::app);
    if (!resultFile.is_open()) {
        std::cout << "Testcase : Failed to open Results.csv file";
        std::cout << std::endl;
        PrintCurrentTrace("main: failed to open results file");
        return -1;
    }

    resultFile << "Testapp Version " << TEST_APP_VERSION << " ";

    while (1) {
        int optIndex = 0;
        static struct option longOpts[] = {
            {"help",        no_argument,       0,  'h' },
            {"config",      required_argument, 0,  'c' },
            {"results",     optional_argument, 0,  'r' },
            {"loglevel",    optional_argument, 0,  'l' },
            {0,             0,                 0,   0  }
        };

        int opt = getopt_long(argc, argv, "h:c:l:r:",
                longOpts, &optIndex);

        if (opt == -1) {
            break;
        }

        switch (opt) {
            case 'h':
                showUsage();
                return 0;
            case 'c':
                configPath = optarg;
                printf("Config file path: %s\n", configPath.c_str());
                break;
            case 'r':
                resultsPath = argv[optind++];
                printf("Results file path: %s\n", resultsPath.c_str());
                break;
            case 'l':
                gLogLevel = atoi(argv[optind++]);
                printf("Log Level : 0x%x\n", gLogLevel);
                break;
            default:
                printf("Error: invalid option. Run \"./iris_v4l2_test --help\" for more info.\n");
                PrintCurrentTrace("main: invalid command line option");
                return -1;
        }
    }

    std::string pathToFile;
    std::vector<std::string> matched_files;

    ret = getRegexMatchFileNames(configPath, matched_files, pathToFile);
    if (ret) {
        std::cout << "Testcase : Failed" << std::endl;
        resultFile << "Testcase : Failed" << std::endl;
        PrintCurrentTrace("main: getRegexMatchFileNames failed");
        return ret;
    }

    std::string ExecutionMode = "Sequential";
    std::unordered_map<std::string, ConfigureStruct> mapTestCasesConfig;

    for (const auto& filename : matched_files) {
        std::cout << "parse " << filename << '\n';

        ret = parseJsonConfigs(pathToFile + "/" + filename, ExecutionMode,
                               mapTestCasesConfig);
        if (ret) {
            printf("Error: Json parsing failed - %s\n", filename.c_str());
            PrintCurrentTrace("main: parseJsonConfigs failed");

            std::cout << "Testcase[" << filename << "] : Failed" << std::endl;
            resultFile << "Testcase[ " << filename << "] : Failed" << std::endl;
            break;
        }

        runAndWaitForComplete(ExecutionMode,
                mapTestCasesConfig, std::ref(resultFile));
    }

    std::cout << "Testapp Version " << TEST_APP_VERSION << std::endl;

    return 0;
}
