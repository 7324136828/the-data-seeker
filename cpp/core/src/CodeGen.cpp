#include "CodeGen.h"
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace native_app {

using json = nlohmann::json;

static std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string CodeGen::Generate(const ApiRequest& req, const std::string& language) {
    std::string lang = language;
    std::transform(lang.begin(), lang.end(), lang.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });

    std::string method = HttpMethodToString(req.method);
    std::string url = req.url;

    std::string bodyContent;
    if (req.body.type == BodyType::Json || req.body.type == BodyType::Raw) {
        bodyContent = req.body.content;
    } else if (req.body.type == BodyType::UrlEncoded) {
        for (size_t i = 0; i < req.body.formItems.size(); ++i) {
            if (i > 0) bodyContent += "&";
            bodyContent += req.body.formItems[i].key + "=" + req.body.formItems[i].value;
        }
    }

    if (lang == "curl") {
        std::ostringstream ss;
        ss << "curl -X " << method << " " << ShellQuote(url);
        if (req.options.followRedirects) ss << " \\\n  -L";
        if (!req.options.verifyTls) ss << " \\\n  --insecure";
        for (const auto& h : req.headers) {
            if (h.enabled && !h.key.empty()) {
                ss << " \\\n  -H " << ShellQuote(h.key + ": " + h.value);
            }
        }
        if (!bodyContent.empty()) {
            ss << " \\\n  -d " << ShellQuote(bodyContent);
        }
        return ss.str();
    } else if (lang == "python" || lang == "requests") {
        std::ostringstream ss;
        ss << "import requests\n\n";
        ss << "url = " << json(url).dump() << "\n";
        ss << "headers = {\n";
        for (const auto& h : req.headers) {
            if (h.enabled && !h.key.empty()) {
                ss << "    " << json(h.key).dump() << ": " << json(h.value).dump() << ",\n";
            }
        }
        ss << "}\n";

        if (!bodyContent.empty()) {
            if (req.body.type == BodyType::Json) {
                try {
                    json j = json::parse(bodyContent);
                    ss << "payload = " << j.dump(4) << "\n";
                    ss << "response = requests." << (method == "DELETE" ? "delete" : "") << (method == "GET" ? "get" : "") << (method == "POST" ? "post" : "") << (method == "PUT" ? "put" : "") << (method == "PATCH" ? "patch" : "")
                       << "(url, headers=headers, json=payload, timeout=" << req.options.timeoutSec << ")\n";
                } catch (...) {
                    ss << "data = " << json(bodyContent).dump() << "\n";
                    ss << "response = requests." << (method == "DELETE" ? "delete" : "") << (method == "GET" ? "get" : "") << (method == "POST" ? "post" : "") << (method == "PUT" ? "put" : "") << (method == "PATCH" ? "patch" : "")
                       << "(url, headers=headers, data=data, timeout=" << req.options.timeoutSec << ")\n";
                }
            } else {
                ss << "data = " << json(bodyContent).dump() << "\n";
                ss << "response = requests." << (method == "DELETE" ? "delete" : "") << (method == "GET" ? "get" : "") << (method == "POST" ? "post" : "") << (method == "PUT" ? "put" : "") << (method == "PATCH" ? "patch" : "")
                   << "(url, headers=headers, data=data, timeout=" << req.options.timeoutSec << ")\n";
            }
        } else {
            ss << "response = requests." << (method == "DELETE" ? "delete" : "") << (method == "GET" ? "get" : "") << (method == "POST" ? "post" : "") << (method == "PUT" ? "put" : "") << (method == "PATCH" ? "patch" : "")
               << "(url, headers=headers, timeout=" << req.options.timeoutSec << ")\n";
        }
        ss << "\nprint(response.status_code)\n";
        ss << "print(response.text)\n";
        return ss.str();
    } else if (lang == "javascript" || lang == "fetch") {
        std::ostringstream ss;
        json opt = json::object();
        opt["method"] = method;
        json hdrs = json::object();
        for (const auto& h : req.headers) {
            if (h.enabled && !h.key.empty()) hdrs[h.key] = h.value;
        }
        opt["headers"] = hdrs;
        if (!bodyContent.empty() && method != "GET" && method != "HEAD") {
            opt["body"] = bodyContent;
        }

        ss << "fetch(" << json(url).dump() << ", " << opt.dump(2) << ")\n";
        ss << "  .then(response => response.text())\n";
        ss << "  .then(data => console.log(data))\n";
        ss << "  .catch(error => console.error(error));\n";
        return ss.str();
    } else if (lang == "axios") {
        std::ostringstream ss;
        ss << "const axios = require('axios');\n\n";
        json cfg = json::object();
        cfg["method"] = method;
        cfg["url"] = url;
        json hdrs = json::object();
        for (const auto& h : req.headers) {
            if (h.enabled && !h.key.empty()) hdrs[h.key] = h.value;
        }
        cfg["headers"] = hdrs;
        if (!bodyContent.empty()) {
            try {
                cfg["data"] = json::parse(bodyContent);
            } catch (...) {
                cfg["data"] = bodyContent;
            }
        }
        cfg["timeout"] = static_cast<int>(req.options.timeoutSec * 1000);

        ss << "const config = " << cfg.dump(2) << ";\n\n";
        ss << "axios(config)\n";
        ss << "  .then(response => console.log(response.data))\n";
        ss << "  .catch(error => console.error(error));\n";
        return ss.str();
    }

    return "Unsupported language: " + language;
}

} // namespace native_app
