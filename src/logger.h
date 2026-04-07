#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>

#define LOG Logger::instance()

class Logger {
public:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    class Entry {
    public:
        Entry(Logger& logger) : lock_(logger.mutex_), stream_(logger.stream_) {
            stream_ << logger.make_timestamp() << " ";
        }

        template<typename T>
        Entry(Logger& logger, const T& val) : Entry(logger) {
            stream_ << val;
        }

        ~Entry() {
            stream_ << std::endl;
        }

        template<typename T>
        Entry& operator<<(const T& val) {
            stream_ << val;
            return *this;
        }

    private:
        std::lock_guard<std::mutex> lock_;
        std::ostream& stream_;
    };

    template<typename T>
    Entry operator<<(const T& val) {
        return Entry(*this, val);
    }

private:
    std::string make_timestamp() const {
        using namespace std::chrono;
        const auto now = system_clock::now();
        const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        const std::time_t tt = system_clock::to_time_t(now);

        std::tm tm {};
        localtime_r(&tt, &tm);

        std::ostringstream ts;
        ts << '[' << std::put_time(&tm, "%Y-%m-%d %H:%M") << ':'
           << std::put_time(&tm, "%S") << '.' << std::setw(3) << std::setfill('0')
           << ms.count() << ']';
        return ts.str();
    }

    mutable std::mutex mutex_;
    std::ostream& stream_ = std::cout;
};
