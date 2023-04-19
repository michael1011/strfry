#include <docopt.h>
#include <ranges>
#include <string_view>
#include <tao/json.hpp>
#include <hoytech/protected_queue.h>
#include <hoytech/file_change_monitor.h>

#include "golpe.h"

#include "WriterPipeline.h"
#include "WSConnection.h"
#include "events.h"


static const char USAGE[] =
R"(
    Usage:
      stream <url> [--dir=<dir>]

    Options:
      --dir=<dir>   Direction: down, up, or both [default: down]
)";


std::string trim_prefix(std::string const & str, std::string const & prefix) {
    if (starts_with(str, prefix)) {
        return str.substr(prefix.size());
    }

    return str;
}

std::string trim_ws_prefix(std::string const & str) {
    return trim_prefix(trim_prefix(str, "wss://"), "ws://");
}

void cmd_stream(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string urls = args["<url>"].asString();
    std::string dir = args["--dir"] ? args["--dir"].asString() : "down";

    if (dir != "up" && dir != "down" && dir != "both") throw herr("invalid direction: ", dir, ". Should be one of up/down/both");

    auto split = urls
        | std::ranges::views::split(',')
        | std::ranges::views::transform([](auto&& str) {
            return std::string_view(&*str.begin(), std::ranges::distance(str));
        });

    std::vector<std::unique_ptr<WSConnection>> wss;

    for (auto && url : split) {
        wss.push_back(std::make_unique<WSConnection>(std::string(url)));
    }

    flat_hash_set<std::string> downloadedIds;
    WriterPipeline writer;
    Decompressor decomp;

    for (auto & ws : wss) {
        ws->onConnect = [&]{
            if (dir == "down" || dir == "both") {
                auto encoded = tao::json::to_string(tao::json::value::array({ "REQ", "sub", tao::json::value({ { "limit", 0 } }) }));
                ws->send(encoded);
            }
        };

        ws->onMessage = [&](auto msg, uWS::OpCode, size_t){
            auto origJson = tao::json::from_string(msg);

            if (origJson.is_array()) {
                if (origJson.get_array().size() < 2) throw herr("array too short");

                auto &msgType = origJson.get_array().at(0);
                if (msgType == "EOSE") {
                    return;
                } else if (msgType == "NOTICE") {
                    LW << "NOTICE message: " << tao::json::to_string(origJson);
                    return;
                } else if (msgType == "OK") {
                    if (!origJson.get_array().at(2).get_boolean()) {
                        LW << "Event not written: " << origJson;
                    }
                } else if (msgType == "EVENT") {
                    if (dir == "down" || dir == "both") {
                        if (origJson.get_array().size() < 3) throw herr("array too short");
                        auto &evJson = origJson.at(2);
                        downloadedIds.emplace(from_hex(evJson.at("id").get_string()));
                        LI << "Got event with ID: " << evJson.at("id").get_string();
                        writer.inbox.push_move({ std::move(evJson), EventSourceType::Stream, ws->url });
                    } else {
                        LW << "Unexpected EVENT";
                    }
                } else {
                    throw herr("unexpected first element");
                }
            } else {
                throw herr("unexpected message");
            }
        };
    }

    uint64_t currEventId;

    {
        auto txn = env.txn_ro();
        currEventId = getMostRecentLevId(txn);
    }

    for (auto & ws : wss) {
        ws->onTrigger = [&]{
            if (dir == "down") return;

            auto txn = env.txn_ro();

            env.foreach_Event(txn, [&](auto &ev){
                currEventId = ev.primaryKeyId;

                auto id = std::string(sv(ev.flat_nested()->id()));
                if (downloadedIds.find(id) != downloadedIds.end()) {
                    downloadedIds.erase(id);
                    return true;
                }

                std::string msg = std::string("[\"EVENT\",");
                msg += getEventJson(txn, decomp, ev.primaryKeyId);
                msg += "]";

                ws->send(msg);

                return true;
            }, false, currEventId + 1);
        };
    }

    std::unique_ptr<hoytech::file_change_monitor> dbChangeWatcher;

    if (dir == "up" || dir == "both") {
        dbChangeWatcher = std::make_unique<hoytech::file_change_monitor>(dbDir + "/data.mdb");

        dbChangeWatcher->setDebounce(100);

        dbChangeWatcher->run([&](){
            for (auto & ws : wss) {
                ws->trigger();
            }
        });
    }

    std::vector<std::thread> threads;

    for (auto & ws : wss) {
        threads.emplace_back([&ws]() {
            setThreadName("WS:" + trim_ws_prefix(ws->url));
            ws->run();
        });
    }

    for (auto & thread : threads) {
        thread.join();
    }
}
