#include "session_actions.hpp"

#include <vector>

#include "esp_err.h"

#include "artemis_json.hpp"
#include "camera_actions.hpp"
#include "session_controller.h"

namespace session_actions
{

namespace
{

Reply refuse(const std::string &text)
{
    Reply reply;
    reply.text = text;
    return reply;
}

}  // namespace

Reply request(From from, const std::string &type_name_text, long long media_id,
              const std::string &code)
{
    using namespace camera::action;
    const std::optional<Type> type = parse_type(type_name_text);
    if (!type) {
        return refuse("unknown action '" + type_name_text + "'");
    }
    const Action action{*type, type == Type::delete_media ? media_id : 0};
    const Source source =
        from == From::console ? Source::console : Source::web_ui;
    const std::string problem = validate(action);
    if (!problem.empty()) {
        return refuse(problem);
    }
    if (!allowed(*type, source)) {
        return refuse(std::string("the ") + type_name(*type) +
                      " action is not allowed from here");
    }

    if (needs_confirmation(*type)) {
        const std::string subject =
            *type == Type::delete_media
                ? "delete " + std::to_string(action.media_id)
                : std::string(type_name(*type));
        if (code.empty()) {
            Reply reply;
            reply.needs_code = true;
            reply.code = service::request_confirmation(subject);
            reply.text = *type == Type::format_sd
                             ? "this erases the camera's SD card and cannot "
                               "be undone"
                             : "this deletes the recording and cannot be "
                               "undone";
            return reply;
        }
        const ConfirmGate::Result checked = service::confirm(subject, code);
        if (checked != ConfirmGate::Result::confirmed) {
            return refuse(confirm_result_text(checked));
        }
    }

    const ActionQueue::AddResult added = service::submit(action, source, "");
    Reply reply;
    reply.id = added.id;
    switch (added.kind) {
    case ActionQueue::AddResult::Kind::added:
        break;
    case ActionQueue::AddResult::Kind::duplicate:
        reply.ok = true;
        reply.text = "already queued";
        return reply;
    case ActionQueue::AddResult::Kind::full:
        return refuse("too many actions are waiting");
    case ActionQueue::AddResult::Kind::invalid:
    case ActionQueue::AddResult::Kind::not_allowed:
        return refuse(added.reason);
    }
    reply.ok = true;
    // A photo is followed by a scan, so that the picture is read and pushed;
    // the others by a short session.
    const esp_err_t error = *type == Type::photo
                                ? session_controller_request_scan()
                                : session_controller_request_actions();
    if (error == ESP_OK) {
        reply.session_started = true;
        reply.text = "queued; the camera session has started";
    } else if (error == ESP_ERR_INVALID_STATE) {
        reply.text = "queued; it runs in the next camera session";
    } else {
        reply.text = std::string("queued; the session could not be started: ") +
                     esp_err_to_name(error);
    }
    return reply;
}

std::string to_json(const Reply &reply)
{
    using camera::artemis::json_quote;
    std::string json = std::string("{\"ok\":") + (reply.ok ? "true" : "false");
    if (reply.needs_code) {
        json += ",\"needsCode\":true,\"code\":" + json_quote(reply.code);
    }
    if (reply.id != 0) {
        json += ",\"id\":" + std::to_string(reply.id);
    }
    json += std::string(",\"started\":") +
            (reply.session_started ? "true" : "false");
    json += ",\"text\":" + json_quote(reply.text) + "}";
    return json;
}

std::string queue_json(std::size_t limit)
{
    using camera::artemis::json_quote;
    const std::vector<camera::action::Entry> entries =
        camera::action::service::snapshot();
    const std::size_t first =
        entries.size() > limit ? entries.size() - limit : 0;
    std::string json = "{\"actions\":[";
    for (std::size_t i = first; i < entries.size(); ++i) {
        if (i > first) {
            json += ',';
        }
        json += "{\"id\":" + std::to_string(entries[i].id) + ",\"text\":" +
                json_quote(camera::action::describe(entries[i])) + "}";
    }
    return json + "]}";
}

}  // namespace session_actions
