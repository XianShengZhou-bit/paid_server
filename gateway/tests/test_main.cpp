// review
#include <cstdlib>
#include <iostream>
#include <string>

#include <gtest/gtest.h>

#include "config.hpp"
#include "logger.hpp"
#include "test_support.hpp"

namespace {

// review
std::string takeEnvPathArg(int& argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr || argv[i][0] == '-') {
            continue;
        }
        std::string path = argv[i];
        for (int j = i; j < argc - 1; ++j) {
            argv[j] = argv[j + 1];
        }
        --argc;
        argv[argc] = nullptr;
        return path;
    }
    return {};
}

} // namespace

// review
int main(int argc, char** argv) {
    std::string env_path = takeEnvPathArg(argc, argv);
    if (env_path.empty()) {
        if (const char* from_env = std::getenv("PAYMENT_TEST_ENV_FILE"); from_env != nullptr && from_env[0] != '\0') {
            env_path = from_env;
        }
    }
    if (env_path.empty()) {
        std::cerr << "用法: " << (argc > 0 ? argv[0] : "gateway_tests") << " <env文件路径> [gtest选项...]\n"
                  << "或设置环境变量 PAYMENT_TEST_ENV_FILE 后运行。\n";
        return 1;
    }

    ::testing::InitGoogleTest(&argc, argv);

    gateway_test::envFilePath() = env_path; // 放env文件路径是为了测试的时候获取后端的redis写入权限
    auto& config = const_cast<payment_config::Config&>(payment_config::Config::instance());
    config.loadFromEnvFile(env_path);

    const auto logger_config = config.logger();
    logger::Logger::getInstance().init(false, logger_config.file, logger_config.max_file_size, logger_config.max_files,
                                       logger_config.console_enabled);

    return RUN_ALL_TESTS();
}
