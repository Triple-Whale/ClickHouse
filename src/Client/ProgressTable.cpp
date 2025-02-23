#include "ProgressTable.h"
#include "Common/AllocatorWithMemoryTracking.h"
#include "Common/ProfileEvents.h"
#include "base/defines.h"

#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <Core/Block.h>
#include <IO/Operators.h>
#include <IO/WriteBufferFromFileDescriptor.h>
#include <Interpreters/ProfileEventsExt.h>
#include <base/terminalColors.h>
#include <Common/TerminalSize.h>
#include <Common/formatReadable.h>

#include <format>
#include <mutex>
#include <numeric>
#include <unordered_map>

namespace DB
{

namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
}

namespace
{

constexpr UInt64 THREAD_GROUP_ID = 0;

constexpr std::string_view CLEAR_TO_END_OF_LINE = "\033[K";
constexpr std::string_view CLEAR_TO_END_OF_SCREEN = "\033[0J";
constexpr std::string_view RESET_COLOR = "\033[0m";
constexpr std::string_view HIDE_CURSOR = "\033[?25l";
constexpr std::string_view SHOW_CURSOR = "\033[?25h";

std::string moveUpNLines(size_t N)
{
    return std::format("\033[{}A", N);
}

std::string formatReadableValue(ProfileEvents::ValueType value_type, double value)
{
    switch (value_type)
    {
        case ProfileEvents::ValueType::Number:
            return formatReadableQuantity(value, /*precision*/ std::floor(value) == value && fabs(value) < 1000 ? 0 : 2);
        case ProfileEvents::ValueType::Bytes:
            return formatReadableSizeWithDecimalSuffix(value);
        case ProfileEvents::ValueType::Nanoseconds:
            return formatReadableTime(value);
        case ProfileEvents::ValueType::Microseconds:
            return formatReadableTime(value * 1e3);
        case ProfileEvents::ValueType::Milliseconds:
            return formatReadableTime(value * 1e6);
    }
}

const std::unordered_map<std::string_view, ProfileEvents::Event> & getEventNameToEvent()
{
    /// TODO: MemoryTracker::USAGE_EVENT_NAME and PEAK_USAGE_EVENT_NAME
    static std::unordered_map<std::string_view, ProfileEvents::Event> event_name_to_event;

    if (!event_name_to_event.empty())
        return event_name_to_event;

    for (ProfileEvents::Event event = ProfileEvents::Event(0); event < ProfileEvents::end(); ++event)
    {
        event_name_to_event.emplace(ProfileEvents::getName(event), event);
    }

    return event_name_to_event;
}


std::string_view setColorForProgress(double progress, double max_progress)
{
    constexpr std::array<std::string_view, 5> colors = {
        "\033[38;5;236m", /// Dark Grey
        "\033[38;5;250m", /// Light Grey
        "\033[38;5;34m", /// Green
        "\033[38;5;226m", /// Yellow
        "\033[1;33m", /// Bold
    };

    constexpr std::array<double, 4> fractions = {
        0.05,
        0.20,
        0.80,
        0.95,
    };

    if (max_progress == 0)
        return colors.front();

    auto fraction = progress / max_progress;
    auto dist = std::upper_bound(fractions.begin(), fractions.end(), fraction) - fractions.begin();
    return colors[dist];
}

std::string_view setColorForBytesBasedMetricsProgress(double progress)
{
    constexpr std::array<std::string_view, 7> colors = {
        "\033[38;5;236m", /// Dark Grey
        "\033[38;5;250m", /// Light Grey
        "\033[38;5;34m", /// Green
        "\033[38;5;226m", /// Yellow
        "\033[38;5;208m", /// Orange
        "\033[1;33m", /// Bold
        "\033[38;5;160m", /// Red: corresponds to >= 1T/s. Not a practical scenario.
    };

    /// Bytes.
    constexpr std::array<uint64_t, 6> thresholds = {
        1LL << 20,
        100LL << 20,
        1'000LL << 20,
        10'000LL << 20,
        100'000LL << 20,
        1'000'000LL << 20,
    };

    auto dist = std::upper_bound(thresholds.begin(), thresholds.end(), progress) - thresholds.begin();
    return colors[dist];
}

std::string_view setColorForTimeBasedMetricsProgress(ProfileEvents::ValueType value_type, double progress)
{
    /// Time units in a second.
    auto units = [](ProfileEvents::ValueType t) -> double
    {
        switch (t)
        {
            case ProfileEvents::ValueType::Milliseconds:
                return 1e3;
            case ProfileEvents::ValueType::Microseconds:
                return 1e6;
            case ProfileEvents::ValueType::Nanoseconds:
                return 1e9;
            default:
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Wrong value type, expecting time units");
        }
    }(value_type);

    constexpr std::array<std::string_view, 5> colors = {
        "\033[38;5;236m", /// Dark Grey
        "\033[38;5;250m", /// Light Grey
        "\033[38;5;34m", /// Green
        "\033[38;5;226m", /// Yellow
        "\033[1;33m" /// Bold
    };

    const std::array<double, 4> thresholds = {0.001 * units, 0.01 * units, 0.1 * units, 1.0 * units};

    auto dist = std::upper_bound(thresholds.begin(), thresholds.end(), progress) - thresholds.begin();
    return colors[dist];
}

std::string_view setColorForDocumentation()
{
    return "\033[38;5;236m"; /// Dark Grey
}

template <typename Out>
void writeWithWidth(Out & out, std::string_view s, size_t width)
{
    if (s.size() >= width)
        out << s << " ";
    else
        out << s << std::string(width - s.size(), ' ');
}

template <typename Out>
void writeWithWidthStrict(Out & out, std::string_view s, size_t width)
{
    constexpr std::string_view ellipsis = "…";
    if (s.size() > width)
        if (width <= ellipsis.size())
            out << s.substr(0, width);
        else
            out << s.substr(0, width - ellipsis.size()) << ellipsis;
    else
        out << s;
}

}

void ProgressTable::writeTable(
    WriteBufferFromFileDescriptor & message, std::unique_lock<std::mutex> &, bool show_table, bool toggle_enabled)
{
    std::lock_guard lock{mutex};
    if (!show_table && toggle_enabled)
    {
        message << CLEAR_TO_END_OF_SCREEN;

        message << HIDE_CURSOR;
        message << "\n";
        message << "Press the space key to toggle the display of the progress table.";
        message << moveUpNLines(1);
        message.next();
        return;
    }

    const auto & event_name_to_event = getEventNameToEvent();

    size_t terminal_width = getTerminalWidth(in_fd, err_fd);
    if (terminal_width < column_event_name_width + COLUMN_VALUE_WIDTH + COLUMN_PROGRESS_WIDTH)
        return;

    if (metrics.empty())
        return;

    message << HIDE_CURSOR;
    message << "\n";
    writeWithWidth(message, COLUMN_EVENT_NAME, column_event_name_width);
    writeWithWidth(message, COLUMN_VALUE, COLUMN_VALUE_WIDTH);
    writeWithWidth(message, COLUMN_PROGRESS, COLUMN_PROGRESS_WIDTH);
    auto col_doc_width = getColumnDocumentationWidth(terminal_width);
    if (col_doc_width)
        writeWithWidth(message, COLUMN_DOCUMENTATION_NAME, col_doc_width);
    message << CLEAR_TO_END_OF_LINE;

    double elapsed_sec = watch.elapsedSeconds();

    for (auto & [name, per_host_info] : metrics)
    {
        if (!per_host_info.isFresh(elapsed_sec))
            continue;

        message << "\n";
        writeWithWidth(message, name, column_event_name_width);

        auto value = per_host_info.getSummaryValue();
        auto value_type = getValueType(event_name_to_event.at(name));
        writeWithWidth(message, formatReadableValue(value_type, value), COLUMN_VALUE_WIDTH);

        /// Get the maximum progress before it is updated in getSummaryProgress.
        auto max_progress = per_host_info.getMaxProgress();
        auto progress = per_host_info.getSummaryProgress(elapsed_sec);
        switch (value_type)
        {
            case ProfileEvents::ValueType::Number:
                message << setColorForProgress(progress, max_progress);
                break;
            case ProfileEvents::ValueType::Bytes:
                message << setColorForBytesBasedMetricsProgress(progress);
                break;
            case ProfileEvents::ValueType::Milliseconds:
                [[fallthrough]];
            case ProfileEvents::ValueType::Microseconds:
                [[fallthrough]];
            case ProfileEvents::ValueType::Nanoseconds:
                message << setColorForTimeBasedMetricsProgress(value_type, progress);
                break;
        }

        writeWithWidth(message, formatReadableValue(value_type, progress) + "/s", COLUMN_PROGRESS_WIDTH);

        if (col_doc_width)
        {
            message << setColorForDocumentation();
            const auto * doc = getDocumentation(event_name_to_event.at(name));
            writeWithWidthStrict(message, doc, col_doc_width);
        }

        message << RESET_COLOR;
        message << CLEAR_TO_END_OF_LINE;
    }

    message << moveUpNLines(getFreshMetricsCount(elapsed_sec));
    message.next();
}

void ProgressTable::writeFinalTable()
{
    std::lock_guard lock{mutex};
    const auto & event_name_to_event = getEventNameToEvent();

    size_t terminal_width = getTerminalWidth(in_fd, err_fd);
    if (terminal_width < column_event_name_width + COLUMN_VALUE_WIDTH)
        return;

    if (metrics.empty())
        return;

    output_stream << "\n";
    writeWithWidth(output_stream, COLUMN_EVENT_NAME, column_event_name_width);
    writeWithWidth(output_stream, COLUMN_VALUE, COLUMN_VALUE_WIDTH);

    for (auto & [name, per_host_info] : metrics)
    {
        output_stream << "\n";
        writeWithWidth(output_stream, name, column_event_name_width);

        auto value = per_host_info.getSummaryValue();
        auto value_type = getValueType(event_name_to_event.at(name));
        writeWithWidth(output_stream, formatReadableValue(value_type, value), COLUMN_VALUE_WIDTH);
    }
}

void ProgressTable::updateTable(const Block & block)
{
    const auto & array_thread_id = typeid_cast<const ColumnUInt64 &>(*block.getByName("thread_id").column).getData();
    const auto & names = typeid_cast<const ColumnString &>(*block.getByName("name").column);
    const auto & host_names = typeid_cast<const ColumnString &>(*block.getByName("host_name").column);
    const auto & array_values = typeid_cast<const ColumnInt64 &>(*block.getByName("value").column).getData();
    const auto & array_type = typeid_cast<const ColumnInt8 &>(*block.getByName("type").column).getData();

    const double time_now = watch.elapsedSeconds();
    size_t max_event_name_width = COLUMN_EVENT_NAME.size();

    std::lock_guard lock{mutex};
    const auto & event_name_to_event = getEventNameToEvent();

    std::vector<std::pair<std::string, size_t>> name_and_row_num_list;
    for (size_t row_num = 0, rows = block.rows(); row_num < rows; ++row_num)
    {
        auto thread_id = array_thread_id[row_num];
        if (thread_id != THREAD_GROUP_ID)
            continue;

        auto name = names.getDataAt(row_num).toString();
        name_and_row_num_list.emplace_back(name, row_num);
    }
    /// Sort by metric name in reverse order, as the most recently updated entries are promoted to the front
    /// of the metric's list.
    std::sort(
        name_and_row_num_list.begin(),
        name_and_row_num_list.end(),
        [](const auto & a, const auto & b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });

    for (const auto & [name, row_num] : name_and_row_num_list)
    {
        auto thread_id = array_thread_id[row_num];

        /// In ProfileEvents packets thread id 0 specifies common profiling information
        /// for all threads executing current query on specific host. So instead of summing per thread
        /// consumption it's enough to look for data with thread id 0.
        if (thread_id != THREAD_GROUP_ID)
            continue;

        chassert(name == names.getDataAt(row_num).toString());

        auto value = array_values[row_num];
        auto host_name = host_names.getDataAt(row_num).toString();
        auto type = static_cast<ProfileEvents::Type>(array_type[row_num]);

        /// Got unexpected event name.
        if (!event_name_to_event.contains(name))
            continue;

        /// Store non-zero values.
        if (value == 0)
            continue;

        auto it = metrics_iterators.find(name);
        if (it == metrics_iterators.end())
        {
            metrics.emplace_front(name, MetricInfoPerHost{});
            metrics_iterators.emplace(name, metrics.begin());
        }
        else
            metrics.splice(metrics.begin(), metrics, it->second);

        metrics.front().second.updateHostValue(host_name, type, value, time_now);

        max_event_name_width = std::max(max_event_name_width, name.size());
    }

    column_event_name_width = max_event_name_width + 1;
}

void ProgressTable::clearTableOutput(WriteBufferFromFileDescriptor & message, std::unique_lock<std::mutex> &)
{
    message << "\r" << CLEAR_TO_END_OF_SCREEN << SHOW_CURSOR;
    message.next();
}

void ProgressTable::resetTable()
{
    std::lock_guard lock{mutex};
    watch.restart();
    metrics.clear();
    metrics_iterators.clear();
}

size_t ProgressTable::getFreshMetricsCount(double time_now) const
{
    auto count = std::count_if(
        metrics.cbegin(),
        metrics.cend(),
        [&time_now](const auto & elem)
        {
            const auto & per_host_info = elem.second;
            return per_host_info.isFresh(time_now);
        });

    /// Number of lines + header.
    return count == 0 ? 0 : count + 1;
}

size_t ProgressTable::getColumnDocumentationWidth(size_t terminal_width) const
{
    auto fixed_columns_width = column_event_name_width + COLUMN_VALUE_WIDTH + COLUMN_PROGRESS_WIDTH;
    if (terminal_width < fixed_columns_width + COLUMN_DOCUMENTATION_MIN_WIDTH)
        return 0;
    return terminal_width - fixed_columns_width;
}

ProgressTable::MetricInfo::MetricInfo(ProfileEvents::Type t) : type(t)
{
}

void ProgressTable::MetricInfo::updateValue(Int64 new_value, double new_time)
{
    /// If the value has not been updated for a long time,
    /// reset the time in snapshots to one second ago.
    if (new_time - new_snapshot.time >= 0.5 || new_snapshot.time == 0)
    {
        prev_shapshot = {new_snapshot.value, new_time - 1.0};
        cur_shapshot = {new_snapshot.value, new_time - 1.0};
    }

    switch (type)
    {
        case ProfileEvents::Type::INCREMENT:
            new_snapshot.value = new_snapshot.value + new_value;
            break;
        case ProfileEvents::Type::GAUGE:
            new_snapshot.value = new_value;
            break;
    }
    new_snapshot.time = new_time;

    if (new_snapshot.time - cur_shapshot.time >= 0.5)
        prev_shapshot = std::exchange(cur_shapshot, new_snapshot);

    update_time = new_time;
}

bool ProgressTable::MetricInfo::isFresh(double now) const
{
    constexpr double freshness_threshold = 3.0;
    chassert(now >= update_time);
    return update_time != 0 && now - update_time <= freshness_threshold;
}

double ProgressTable::MetricInfo::calculateProgress(double time_now) const
{
    /// If the value has not been updated for a long time, the progress is 0.
    if (time_now - new_snapshot.time >= 0.5)
        return 0;

    return (cur_shapshot.value - prev_shapshot.value) / (cur_shapshot.time - prev_shapshot.time);
}

double ProgressTable::MetricInfo::getValue() const
{
    return new_snapshot.value;
}

void ProgressTable::MetricInfoPerHost::updateHostValue(const HostName & host, ProfileEvents::Type type, Int64 new_value, double new_time)
{
    auto it = host_to_metric.find(host);
    if (it == host_to_metric.end())
        it = host_to_metric.emplace(host, type).first;
    it->second.updateValue(new_value, new_time);
}

double ProgressTable::MetricInfoPerHost::getSummaryValue()
{
    return std::accumulate(
        host_to_metric.cbegin(),
        host_to_metric.cend(),
        0.0,
        [](double acc, const auto & host_data)
        {
            const MetricInfo & info = host_data.second;
            return acc + info.getValue();
        });
}

double ProgressTable::MetricInfoPerHost::getSummaryProgress(double time_now)
{
    auto progress = std::accumulate(
        host_to_metric.cbegin(),
        host_to_metric.cend(),
        0.0,
        [time_now](double acc, const auto & host_data)
        {
            const MetricInfo & info = host_data.second;
            return acc + info.calculateProgress(time_now);
        });
    max_progress = std::max(max_progress, progress);
    return progress;
}

double ProgressTable::MetricInfoPerHost::getMaxProgress() const
{
    return max_progress;
}

bool ProgressTable::MetricInfoPerHost::isFresh(double now) const
{
    return std::any_of(host_to_metric.cbegin(), host_to_metric.cend(), [&now](const auto & p) { return p.second.isFresh(now); });
}
}
