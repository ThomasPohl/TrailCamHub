#pragma once

// The two-step confirmation of an action that cannot be undone (#196): the
// front end asks for a code ("format"), the person repeats it ("format
// confirm 4821") within a minute. The rule lives here so that every front end
// gets the same one. Pure logic with the time and the randomness passed in,
// host-tested.

#include <cstdint>
#include <string>

namespace camera::action
{

class ConfirmGate
{
  public:
    static constexpr std::int64_t kValidityUs = 60 * 1000000LL;
    // Wrong codes before the request is dropped, so that the four digits
    // cannot be tried one after the other.
    static constexpr unsigned kMaxWrongCodes = 3;

    enum class Result {
        confirmed,
        no_request,
        expired,
        wrong_subject,
        wrong_code,
    };

    // Opens a confirmation for `subject` (e.g. "format" or "delete 123") and
    // returns its four-digit code. It replaces any earlier request: only one
    // is open at a time. `entropy` is any random number.
    std::string request(const std::string &subject, std::int64_t now_us,
                        std::uint32_t entropy);

    // Checks a code. A confirmed request is used up; a second try fails.
    Result confirm(const std::string &subject, const std::string &code,
                   std::int64_t now_us);

    // The open request (for the console's status), empty if none.
    const std::string &open_subject() const
    {
        return subject_;
    }

  private:
    std::string subject_;
    std::string code_;
    std::int64_t opened_us_ = 0;
    unsigned wrong_ = 0;
};

// Words for a Result, for the front ends.
const char *confirm_result_text(ConfirmGate::Result result);

}  // namespace camera::action
