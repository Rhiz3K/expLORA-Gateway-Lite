/**
 * Pure security policy helpers shared by firmware and host-native tests.
 *
 * This header deliberately has no Arduino dependencies. Keep parsing and
 * state decisions here small enough to audit and exercise on a workstation.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <string>

namespace SecurityPolicy
{

constexpr char ACCESS_CREDENTIAL_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr std::size_t ACCESS_CREDENTIAL_LENGTH = 20;
constexpr std::size_t ACCESS_CREDENTIAL_MIN_LENGTH = 16;
constexpr std::size_t ACCESS_CREDENTIAL_MAX_LENGTH = 63;

inline bool isAccessCredentialFormat(const std::string &credential)
{
    if (credential.size() < ACCESS_CREDENTIAL_MIN_LENGTH ||
        credential.size() > ACCESS_CREDENTIAL_MAX_LENGTH)
    {
        return false;
    }

    return credential.find_first_not_of(ACCESS_CREDENTIAL_ALPHABET) == std::string::npos;
}

inline std::string htmlEscape(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value)
    {
        switch (ch)
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            escaped += ch;
            break;
        }
    }
    return escaped;
}

inline std::string csvEscape(std::string value)
{
    // Prevent spreadsheet applications from interpreting an attacker-chosen
    // sensor name as a formula when API CSV output is opened interactively.
    if (!value.empty() &&
        (value.front() == '=' || value.front() == '+' || value.front() == '-' ||
         value.front() == '@' || value.front() == '\t'))
    {
        value.insert(value.begin(), '\'');
    }

    bool needsQuotes = false;
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char ch : value)
    {
        if (ch == '"')
        {
            escaped += "\"\"";
            needsQuotes = true;
        }
        else
        {
            escaped += ch;
            needsQuotes = needsQuotes || ch == ',' || ch == '\r' || ch == '\n';
        }
    }
    return needsQuotes ? "\"" + escaped + "\"" : escaped;
}

inline std::string resolveSecret(const std::string &existing,
                                 const std::string &submitted,
                                 bool clearRequested)
{
    if (clearRequested)
    {
        return "";
    }
    return submitted.empty() ? existing : submitted;
}

inline bool isHexString(const std::string &value, std::size_t exactLength)
{
    if (value.size() != exactLength)
    {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

inline std::string asciiLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline bool hasUnsafeUrlCharacter(const std::string &value)
{
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch <= 0x20 || ch == 0x7f || ch == '\\';
    });
}

/**
 * Allow only release artifacts below the official HTTPS directory.
 *
 * Percent-encoded paths are intentionally rejected instead of normalized:
 * OTA URLs are machine-generated and have no legitimate need for ambiguous
 * path encoding. Query strings are allowed; fragments are not sent to a
 * server and are rejected to keep policy decisions unambiguous.
 */
inline bool isAllowedFirmwareUrl(const std::string &url)
{
    static const std::string scheme = "https://";
    static const std::string requiredHost = "pajenicko.cz";
    static const std::string requiredPathPrefix = "/webflash/explora-gw-lite/";

    if (url.compare(0, scheme.size(), scheme) != 0 || hasUnsafeUrlCharacter(url) ||
        url.find('#') != std::string::npos)
    {
        return false;
    }

    const std::size_t authorityStart = scheme.size();
    const std::size_t pathStart = url.find('/', authorityStart);
    if (pathStart == std::string::npos)
    {
        return false;
    }

    std::string authority = url.substr(authorityStart, pathStart - authorityStart);
    if (authority.empty() || authority.find('@') != std::string::npos ||
        authority.find('[') != std::string::npos || authority.find(']') != std::string::npos)
    {
        return false;
    }

    std::string host = authority;
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos)
    {
        host = authority.substr(0, colon);
        const std::string port = authority.substr(colon + 1);
        if (port != "443")
        {
            return false;
        }
    }

    if (asciiLower(host) != requiredHost)
    {
        return false;
    }

    const std::size_t queryStart = url.find('?', pathStart);
    const std::string path = url.substr(
        pathStart,
        queryStart == std::string::npos ? std::string::npos : queryStart - pathStart);
    if (path.compare(0, requiredPathPrefix.size(), requiredPathPrefix) != 0 ||
        path.find('%') != std::string::npos || path.find("//") != std::string::npos)
    {
        return false;
    }

    std::size_t segmentStart = 0;
    while (segmentStart < path.size())
    {
        const std::size_t segmentEnd = path.find('/', segmentStart);
        const std::string segment = path.substr(
            segmentStart,
            segmentEnd == std::string::npos ? std::string::npos : segmentEnd - segmentStart);
        if (segment == "." || segment == "..")
        {
            return false;
        }
        if (segmentEnd == std::string::npos)
        {
            break;
        }
        segmentStart = segmentEnd + 1;
    }

    return path.size() > requiredPathPrefix.size();
}

/**
 * The release manifest intentionally lives at a legacy flat path while
 * firmware artifacts live below the narrower directory accepted above.
 * Keep the policies separate so widening the manifest rule can never widen
 * the artifact download surface.
 */
inline bool isAllowedManifestUrl(const std::string &url)
{
    static const std::string allowed =
        "https://pajenicko.cz/webflash/explora-gw-lite-autoupdate.json";
    return !hasUnsafeUrlCharacter(url) && url.find('#') == std::string::npos &&
           asciiLower(url) == allowed;
}

struct SemanticVersion
{
    unsigned long major = 0;
    unsigned long minor = 0;
    unsigned long patch = 0;
    bool prerelease = false;
    std::string prereleaseText;
    bool valid = false;
};

inline bool parseVersionNumber(const std::string &text, std::size_t &position,
                               unsigned long &result)
{
    if (position >= text.size() ||
        std::isdigit(static_cast<unsigned char>(text[position])) == 0)
    {
        return false;
    }

    const std::size_t start = position;
    result = 0;
    while (position < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[position])) != 0)
    {
        const unsigned long digit =
            static_cast<unsigned long>(text[position] - '0');
        if (result > (std::numeric_limits<unsigned long>::max() - digit) / 10UL)
        {
            return false;
        }
        result = result * 10UL + digit;
        ++position;
    }
    if (position - start > 1 && text[start] == '0')
    {
        return false;
    }
    return true;
}

inline bool isValidIdentifierList(const std::string &text,
                                  std::size_t start,
                                  std::size_t end,
                                  bool rejectNumericLeadingZeros)
{
    if (start >= end)
    {
        return false;
    }

    std::size_t identifierStart = start;
    for (std::size_t position = start; position <= end; ++position)
    {
        if (position < end)
        {
            const unsigned char ch = static_cast<unsigned char>(text[position]);
            if (ch != '.' &&
                !(std::isalnum(ch) != 0 || ch == '-'))
            {
                return false;
            }
        }

        if (position == end || text[position] == '.')
        {
            if (position == identifierStart)
            {
                return false;
            }
            if (rejectNumericLeadingZeros &&
                position - identifierStart > 1 &&
                text[identifierStart] == '0' &&
                std::all_of(
                    text.begin() + identifierStart,
                    text.begin() + position,
                    [](unsigned char ch) { return std::isdigit(ch) != 0; }))
            {
                return false;
            }
            identifierStart = position + 1;
        }
    }
    return true;
}

inline SemanticVersion parseSemanticVersion(const std::string &text)
{
    SemanticVersion version;
    std::size_t position = 0;
    if (!parseVersionNumber(text, position, version.major) ||
        position >= text.size() || text[position++] != '.' ||
        !parseVersionNumber(text, position, version.minor) ||
        position >= text.size() || text[position++] != '.' ||
        !parseVersionNumber(text, position, version.patch))
    {
        return version;
    }

    if (position < text.size() && text[position] == '-')
    {
        ++position;
        const std::size_t start = position;
        while (position < text.size() && text[position] != '+')
        {
            ++position;
        }
        if (!isValidIdentifierList(text, start, position, true))
        {
            return version;
        }
        version.prerelease = true;
        version.prereleaseText = text.substr(start, position - start);
    }

    if (position < text.size() && text[position] == '+')
    {
        ++position;
        const std::size_t start = position;
        position = text.size();
        if (!isValidIdentifierList(text, start, position, false))
        {
            return version;
        }
    }

    version.valid = position == text.size();
    return version;
}

inline bool isNumericIdentifier(const std::string &identifier)
{
    return !identifier.empty() &&
           std::all_of(identifier.begin(), identifier.end(),
                       [](unsigned char ch) {
                           return std::isdigit(ch) != 0;
                       });
}

inline int comparePrerelease(const std::string &left,
                             const std::string &right)
{
    std::size_t leftStart = 0;
    std::size_t rightStart = 0;
    while (true)
    {
        const std::size_t leftEnd = left.find('.', leftStart);
        const std::size_t rightEnd = right.find('.', rightStart);
        const std::string leftIdentifier = left.substr(
            leftStart, leftEnd == std::string::npos
                           ? std::string::npos
                           : leftEnd - leftStart);
        const std::string rightIdentifier = right.substr(
            rightStart, rightEnd == std::string::npos
                            ? std::string::npos
                            : rightEnd - rightStart);

        const bool leftNumeric = isNumericIdentifier(leftIdentifier);
        const bool rightNumeric = isNumericIdentifier(rightIdentifier);
        if (leftNumeric && rightNumeric)
        {
            if (leftIdentifier.size() != rightIdentifier.size())
            {
                return leftIdentifier.size() > rightIdentifier.size() ? 1 : -1;
            }
        }
        else if (leftNumeric != rightNumeric)
        {
            // SemVer gives numeric identifiers lower precedence than
            // non-numeric identifiers.
            return leftNumeric ? -1 : 1;
        }

        if (leftIdentifier != rightIdentifier)
        {
            return leftIdentifier > rightIdentifier ? 1 : -1;
        }

        const bool leftFinished = leftEnd == std::string::npos;
        const bool rightFinished = rightEnd == std::string::npos;
        if (leftFinished || rightFinished)
        {
            if (leftFinished && rightFinished)
            {
                return 0;
            }
            // A larger set of otherwise equal identifiers has higher
            // precedence: alpha.1 > alpha.
            return leftFinished ? -1 : 1;
        }
        leftStart = leftEnd + 1;
        rightStart = rightEnd + 1;
    }
}

inline bool isVersionNewer(const std::string &candidateText,
                           const std::string &currentText)
{
    const SemanticVersion candidate = parseSemanticVersion(candidateText);
    const SemanticVersion current = parseSemanticVersion(currentText);
    if (!candidate.valid || !current.valid)
    {
        return false;
    }

    if (candidate.major != current.major)
        return candidate.major > current.major;
    if (candidate.minor != current.minor)
        return candidate.minor > current.minor;
    if (candidate.patch != current.patch)
        return candidate.patch > current.patch;
    if (candidate.prerelease != current.prerelease)
        return current.prerelease && !candidate.prerelease;
    if (!candidate.prerelease)
        return false;
    return comparePrerelease(candidate.prereleaseText,
                             current.prereleaseText) > 0;
}

struct Authority
{
    std::string host;
    unsigned int port = 0;
    bool explicitPort = false;
    bool valid = false;
};

inline Authority parseHttpAuthority(const std::string &rawAuthority, unsigned int defaultPort)
{
    Authority result;
    if (rawAuthority.empty() || hasUnsafeUrlCharacter(rawAuthority) ||
        rawAuthority.find('@') != std::string::npos)
    {
        return result;
    }

    std::string host;
    std::string portText;
    if (rawAuthority.front() == '[')
    {
        const std::size_t close = rawAuthority.find(']');
        if (close == std::string::npos)
        {
            return result;
        }
        host = rawAuthority.substr(0, close + 1);
        if (close + 1 < rawAuthority.size())
        {
            if (rawAuthority[close + 1] != ':')
            {
                return result;
            }
            portText = rawAuthority.substr(close + 2);
        }
    }
    else
    {
        const std::size_t firstColon = rawAuthority.find(':');
        const std::size_t lastColon = rawAuthority.rfind(':');
        if (firstColon != lastColon)
        {
            return result;
        }
        if (lastColon == std::string::npos)
        {
            host = rawAuthority;
        }
        else
        {
            host = rawAuthority.substr(0, lastColon);
            portText = rawAuthority.substr(lastColon + 1);
        }
    }

    if (host.empty())
    {
        return result;
    }

    unsigned int port = defaultPort;
    if (!portText.empty())
    {
        result.explicitPort = true;
        unsigned int parsed = 0;
        for (const unsigned char ch : portText)
        {
            if (std::isdigit(ch) == 0)
            {
                return result;
            }
            const unsigned int digit = static_cast<unsigned int>(ch - '0');
            if (parsed > (65535U - digit) / 10U)
            {
                return result;
            }
            parsed = parsed * 10U + digit;
        }
        if (parsed == 0)
        {
            return result;
        }
        port = parsed;
    }
    else if (rawAuthority.back() == ':')
    {
        return result;
    }

    result.host = asciiLower(host);
    result.port = port;
    result.valid = true;
    return result;
}

/**
 * Browser POSTs must come from the exact HTTP origin serving the request.
 * An absent Origin is accepted for authenticated command-line/device clients.
 *
 * The gateway itself serves plain HTTP, so a Host header without an explicit
 * port means port 80. An HTTPS reverse proxy must preserve its external port
 * in Host (for example gateway.local:443); otherwise the gateway cannot
 * distinguish that proxy from a forged scheme/port combination.
 */
inline bool isAllowedBrowserOrigin(const std::string &origin, const std::string &hostHeader)
{
    if (origin.empty())
    {
        return true;
    }

    static const std::string httpScheme = "http://";
    static const std::string httpsScheme = "https://";
    unsigned int defaultPort = 0;
    std::size_t schemeLength = 0;
    if (origin.compare(0, httpScheme.size(), httpScheme) == 0)
    {
        defaultPort = 80;
        schemeLength = httpScheme.size();
    }
    else if (origin.compare(0, httpsScheme.size(), httpsScheme) == 0)
    {
        defaultPort = 443;
        schemeLength = httpsScheme.size();
    }
    else
    {
        return false;
    }

    if (hasUnsafeUrlCharacter(origin))
    {
        return false;
    }

    const std::string originAuthorityText = origin.substr(schemeLength);
    if (originAuthorityText.empty() || originAuthorityText.find('/') != std::string::npos ||
        originAuthorityText.find('?') != std::string::npos ||
        originAuthorityText.find('#') != std::string::npos)
    {
        return false;
    }

    const Authority originAuthority =
        parseHttpAuthority(originAuthorityText, defaultPort);
    const Authority requestAuthority =
        parseHttpAuthority(hostHeader, 80);
    return originAuthority.valid && requestAuthority.valid &&
           originAuthority.host == requestAuthority.host &&
           originAuthority.port == requestAuthority.port;
}

enum class ApLifecycleEvent
{
    StaDisconnected,
    StaReconnected,
    TemporaryWindowExpired
};

enum class ApLifecycleAction
{
    KeepCurrent,
    EnsureRunning,
    ShutDown
};

inline ApLifecycleAction decideApLifecycleAction(ApLifecycleEvent event, bool staConnected)
{
    switch (event)
    {
    case ApLifecycleEvent::StaDisconnected:
        return ApLifecycleAction::EnsureRunning;
    case ApLifecycleEvent::StaReconnected:
        return staConnected ? ApLifecycleAction::ShutDown : ApLifecycleAction::KeepCurrent;
    case ApLifecycleEvent::TemporaryWindowExpired:
        return staConnected ? ApLifecycleAction::ShutDown : ApLifecycleAction::KeepCurrent;
    }
    return ApLifecycleAction::KeepCurrent;
}

} // namespace SecurityPolicy
