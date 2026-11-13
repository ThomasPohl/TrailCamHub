#include "action_confirm.hpp"

namespace camera::action
{

std::string ConfirmGate::request(const std::string &subject,
                                 std::int64_t now_us, std::uint32_t entropy)
{
    subject_ = subject;
    code_ = std::to_string(1000 + entropy % 9000);
    opened_us_ = now_us;
    wrong_ = 0;
    return code_;
}

ConfirmGate::Result ConfirmGate::confirm(const std::string &subject,
                                         const std::string &code,
                                         std::int64_t now_us)
{
    if (subject_.empty()) {
        return Result::no_request;
    }
    if (now_us - opened_us_ > kValidityUs) {
        subject_.clear();
        return Result::expired;
    }
    if (subject != subject_) {
        return Result::wrong_subject;
    }
    if (code != code_) {
        if (++wrong_ >= kMaxWrongCodes) {
            subject_.clear();
        }
        return Result::wrong_code;
    }
    subject_.clear();
    return Result::confirmed;
}

const char *confirm_result_text(ConfirmGate::Result result)
{
    switch (result) {
    case ConfirmGate::Result::confirmed:
        return "confirmed";
    case ConfirmGate::Result::no_request:
        return "nothing to confirm; start it again";
    case ConfirmGate::Result::expired:
        return "the code expired; start it again";
    case ConfirmGate::Result::wrong_subject:
        return "the code belongs to another action";
    case ConfirmGate::Result::wrong_code:
        return "wrong code";
    }
    return "";
}

}  // namespace camera::action
