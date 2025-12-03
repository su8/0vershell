/*
 * Copyright 12/03/2025 https://github.com/su8/0vershell
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

// Expand variables in a string
std::string expandVariables(const std::string &input,
                            const std::unordered_map<std::string, std::string> &vars) {
    std::regex varPattern(R"(\$([A-Za-z_][A-Za-z0-9_]*))");
    return std::regex_replace(input, varPattern, [&](const std::smatch &m) {
        std::string varName = m[1];
        auto it = vars.find(varName);
        return (it != vars.end()) ? it->second : "";
    });
}

// Parse echo arguments with Bash-like quoting rules
std::string parseEchoArgs(const std::string &args,
                          const std::unordered_map<std::string, std::string> &vars) {
    std::string result;
    bool inSingle = false, inDouble = false;
    std::string token;

    for (size_t i = 0; i < args.size(); ++i) {
        char c = args[i];

        if (c == '\'' && !inDouble) { // toggle single quotes
            inSingle = !inSingle;
            continue;
        }
        if (c == '"' && !inSingle) { // toggle double quotes
            inDouble = !inDouble;
            continue;
        }

        if (!inSingle && !inDouble && std::isspace(static_cast<unsigned char>(c))) {
            // End of token
            if (!token.empty()) {
                result += expandVariables(token, vars);
                token.clear();
            }
            result += ' '; // preserve space
        } else {
            token += c;
        }
    }

    // Process last token
    if (!token.empty()) {
        if (inSingle) {
            result += token; // literal
        } else {
            result += expandVariables(token, vars);
        }
    }

    return result;
}

in main()

if (cmd2 == "echo") {
            std::string args;
            std::getline(iss, args);
            args = std::regex_replace(args, std::regex("^ +"), "");
            std::cout << parseEchoArgs(args, variables) << "\n";
        }