"""
Limits for ESPAsyncWebServer requests before the login check

The library reads request headers before any handler or login check runs and
keeps no limit: endless header lines, many headers or many idle half-sent
requests exhaust the heap (and a failed allocation of a header ends in
std::bad_alloc - an abort). The middleware below hands the compiler a patched
copy of WebRequest.cpp; the downloaded library stays untouched:

- a header line is at most MAX_LINE bytes, at most MAX_HEADERS headers and
  MAX_HEADER_BYTES in total are kept per request (further ones are dropped);
  the Cookie header (the login session) is always kept - Chromium sends it
  last, after 16+ other headers
- at most MAX_PARAMS query/form parameters are kept per request (each one
  is a heap node: a 4 kB body of '&' made ~4000 of them)
- multipart loops over the boundary use size_t (a uint8_t index never ended
  for boundaries over 255 characters)
- a new request is refused while MAX_ACTIVE_REQUESTS are already being
  received (counted from their first byte, so idle browser preconnects do
  not count; idle connections close after the library's own 3 s rx timeout)
- a request (other than a logged-in upload) is closed after
  MAX_REQUEST_LIFETIME_MS in total, also while its response is being sent -
  a client acking a few bytes at a time cannot hold a slot for good
- multipart field values and part headers are capped at MAX_PART_BUFFER
- a request still being received is dropped when the heap runs low
  (MIN_FREE_HEAP / MIN_FREE_BLOCK) - the bound that holds whatever the other
  limits let through - or after RECEIVE_DEADLINE_MS (a slow trickle resets
  the rx timeout); an authorized firmware upload is exempt from both (its
  data streams to flash), an oversized body without login is dropped at once
  (OversizeRequestHandler marks it)
"""

import os

Import("env")  # noqa: F821

MAX_LINE = 2048  # a Cookie header for the gateway's host can be long
MAX_HEADERS = 16
MAX_HEADER_BYTES = 4096
MAX_PARAMS = 32
MAX_ACTIVE_REQUESTS = 6
MIN_FREE_HEAP = 30000
MIN_FREE_BLOCK = 8192
RECEIVE_DEADLINE_MS = 10000
MAX_FORM_BODY = 4096  # must match MAX_REQUEST_BODY in src/Web/WebServer.cpp
MAX_REQUEST_LIFETIME_MS = 30000  # any request but a logged-in upload
MAX_PART_BUFFER = 2048  # multipart field values and part headers

PATCHES = [
    # A line without newline: refuse to grow past MAX_LINE
    (
        "        if (!_temp.reserve(_temp.length() + len)) {",
        "        if (_temp.length() + len > %d) {\n          EXPLORA_REJECT();\n        }\n"
        "        if (!_temp.reserve(_temp.length() + len)) {" % MAX_LINE,
    ),
    # A complete line that is too long
    (
        "        str[i] = 0;  // Terminate the string at the end of the line.\n        _temp.concat(str);\n",
        "        str[i] = 0;  // Terminate the string at the end of the line.\n        _temp.concat(str);\n"
        "        if (_temp.length() > %d) {\n          EXPLORA_REJECT();\n        }\n"
        % MAX_LINE,
    ),
    # Extra headers are dropped once the count or byte budget is used up
    (
        "    _headers.emplace_back(name, value);",
        "    size_t headerBytes = name.length() + value.length();\n"
        "    for (const auto &h : _headers) {\n"
        "      headerBytes += h.name().length() + h.value().length();\n"
        "    }\n"
        "    bool firstCookie = name.equalsIgnoreCase(\"Cookie\") && !hasHeader(\"Cookie\");\n"
        "    if (firstCookie || (_headers.size() < %d && headerBytes <= %d)) {\n"
        "      _headers.emplace_back(name, value);\n    }"
        % (MAX_HEADERS, MAX_HEADER_BYTES),
    ),
    # Parameters: query string, form body, multipart fields and files
    (
        "    if (name.length()) {\n      _params.emplace_back(name, value);\n    }",
        "    if (name.length() && _params.size() < %d) {\n      _params.emplace_back(name, value);\n    }" % MAX_PARAMS,
    ),
    (
        "    if (name.length()) {\n      _params.emplace_back(name, urlDecode(value), true);\n    }",
        "    if (name.length() && _params.size() < %d) {\n      _params.emplace_back(name, urlDecode(value), true);\n    }"
        % MAX_PARAMS,
    ),
    (
        "      if (!_itemIsFile) {\n        _params.emplace_back(_itemName, _itemValue, true);",
        "      if (!_itemIsFile) {\n        if (_params.size() < %d) _params.emplace_back(_itemName, _itemValue, true);" % MAX_PARAMS,
    ),
    (
        "          _params.emplace_back(_itemName, _itemFilename, true, true, _itemSize);",
        "          if (_params.size() < %d) _params.emplace_back(_itemName, _itemFilename, true, true, _itemSize);"
        % MAX_PARAMS,
    ),
    # Multipart buffers (also of a logged-in upload, exempt from the heap gate)
    (
        "    else                          \\\n      _itemValue += (char)(b);    \\\n",
        "    else if (_itemValue.length() < %d) \\\n      _itemValue += (char)(b);    \\\n" % MAX_PART_BUFFER,
    ),
    (
        "      _temp += (char)data;\n",
        "      if (_temp.length() < %d) _temp += (char)data;\n" % MAX_PART_BUFFER,
    ),
    # Count open requests
    (
        '#include <cstring>\n',
        '#include <cstring>\n\n'
        '// Defined in src/Web/WebServer.cpp (attributes are keyed by pointer)\n'
        'extern const char EXPLORA_REJECT_ATTR[];\n\n'
        '// Rejected requests: drop their buffers now, close the connection from\n'
        '// _onPoll (an abort() inside a data callback frees the client while\n'
        '// AsyncClient::_recv is still running on it)\n'
        '#define EXPLORA_REJECT() do { _parseState = PARSE_REQ_FAIL; _temp = String(); _headers.clear(); '
        '_params.clear(); return; } while (0)\n\n'
        '// Requests that have received data (all of this runs in the AsyncTCP task)\n'
        'struct ActiveRequest {\n  AsyncWebServerRequest *request;\n  uint32_t since;\n};\n'
        'static ActiveRequest s_activeRequests[%d];\n'
        'static ActiveRequest *trackRequest(AsyncWebServerRequest *r) {\n'
        '  for (auto &slot : s_activeRequests) {\n'
        '    if (slot.request == r) return &slot;\n'
        '  }\n'
        '  for (auto &slot : s_activeRequests) {\n'
        '    if (!slot.request) { slot.request = r; slot.since = millis(); return &slot; }\n'
        '  }\n'
        '  return nullptr;\n'
        '}\n'
        'static ActiveRequest *findRequest(AsyncWebServerRequest *r) {\n'
        '  for (auto &slot : s_activeRequests) {\n'
        '    if (slot.request == r) return &slot;\n'
        '  }\n'
        '  return nullptr;\n'
        '}\n'
        'static void untrackRequest(AsyncWebServerRequest *r) {\n'
        '  for (auto &slot : s_activeRequests) {\n'
        '    if (slot.request == r) slot.request = nullptr;\n'
        '  }\n'
        '}\n' % MAX_ACTIVE_REQUESTS,
    ),
    (
        "_itemIsFile(false), _tempObject(NULL) {\n",
        "_itemIsFile(false), _tempObject(NULL) {\n",
    ),
    (
        "AsyncWebServerRequest::~AsyncWebServerRequest() {\n",
        "AsyncWebServerRequest::~AsyncWebServerRequest() {\n  untrackRequest(this);\n",
    ),
    # Rejected requests are closed here (safe: AsyncClient::_poll does not
    # touch the client after its poll callback)
    (
        "void AsyncWebServerRequest::_onPoll() {\n",
        "void AsyncWebServerRequest::_onPoll() {\n"
        "  ActiveRequest *active = findRequest(this);\n"
        "  bool expired = active && _contentLength <= %d && millis() - active->since > %d;\n"
        "  if ((_parseState == PARSE_REQ_FAIL || expired) && _client != NULL) {\n    _client->abort();\n    return;\n  }\n"
        % (MAX_FORM_BODY, MAX_REQUEST_LIFETIME_MS),
    ),
    # Refuse a new request (still at its first bytes) above the limit
    (
        "void AsyncWebServerRequest::_onData(void *buf, size_t len) {\n",
        "void AsyncWebServerRequest::_onData(void *buf, size_t len) {\n"
        "  ActiveRequest *active = trackRequest(this);\n"
        "  bool upload = _parseState >= PARSE_REQ_BODY && _contentLength > %d; // only logged-in uploads get this far\n"
        "  if (_parseState == PARSE_REQ_FAIL) {\n    return; // rejected, waiting for _onPoll to close it\n  }\n"
        "  if (!active || hasAttribute(EXPLORA_REJECT_ATTR) ||\n"
        "      (_parseState < PARSE_REQ_END && !upload &&\n"
        "       (millis() - active->since > %d || ESP.getFreeHeap() < %d || ESP.getMaxAllocHeap() < %d))) {\n"
        "    EXPLORA_REJECT();\n  }\n"
        % (MAX_FORM_BODY, RECEIVE_DEADLINE_MS, MIN_FREE_HEAP, MIN_FREE_BLOCK),
    ),
]


# Applied to every occurrence (three multipart loops)
REPLACE_ALL = [
    ("      uint8_t i;\n      for (i = 0; i < ", "      size_t i;\n      for (i = 0; i < ", 3),
]


def patch_webrequest(env, node):
    path = node.srcnode().get_abspath()
    if os.path.basename(path) != "WebRequest.cpp" or "ESPAsyncWebServer" not in path:
        return node
    with open(path) as f:
        source = f.read()
    for old, new in PATCHES:
        if source.count(old) != 1:
            raise SystemExit("patch_asyncwebserver.py: library changed, patch does not apply: %r" % old[:60])
        source = source.replace(old, new)
    for old, new, count in REPLACE_ALL:
        if source.count(old) != count:
            raise SystemExit("patch_asyncwebserver.py: library changed, patch does not apply: %r" % old[:60])
        source = source.replace(old, new)
    out_dir = os.path.join(env.subst("$BUILD_DIR"), "patched_asyncwebserver")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "WebRequest.cpp")
    current = open(out).read() if os.path.exists(out) else None
    if current != source:
        with open(out, "w") as f:
            f.write(source)
    # Quoted includes of the original directory must still resolve
    return env.Object(
        os.path.join(out_dir, "WebRequest.o"),
        out,
        CPPPATH=env.get("CPPPATH", []) + [os.path.dirname(path)],
    )


env.AddBuildMiddleware(patch_webrequest, "*WebRequest.cpp")  # noqa: F821
