#include "filmer.h"
#include "Extension/Console/commands.h"

// The `filmer` console commands: the only way anything reaches the Filmer, the menu and the binds
// (Filmer on / off, slap the lens) included.
namespace dingosdk::console {
void register_filmer_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
        bool quiet; // sent on every slider movement: not echoed into the console
    };
    const Verb verbs[]{
        {"status", "What the Filmer is doing", nullptr, false},
        {"toggle", "Turn the Filmer on (as last used, or the mode given) or off", "[follow|film]", false},
        {"mode", "off; follow: an AI filmer films a skater; film: you film (right stick aims)", "off|follow|film", false},
        {"subject", "Whom the AI filmer follows", "me|<player>", false},
        {"target", "Whom the assist frames while you film", "auto|<player>", false},
        {"set", "Change a Filmer option",
         "style auto|behind|side|ahead, lens fisheye|long, distance|height|tilt|human|reaction|sensitivity|hold|bend <number>, "
         "assist|invert|recenter|hidehud|stick|clicks|drops|look|anywhere|fisheye|bars|hood|grade|soft|scanlines|grain|date|viewfinder 0|1",
         true},
        {"plant", "The AI filmer stops and films from where they are, or rides again", "[on|off]", false},
        {"slap", "Slap the lens: ends the clip", nullptr, false},
        {"clips", "The clips marked this session", nullptr, false},
        {"reset", "Put every Filmer option back to its default", nullptr, false},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("filmer ") + verb.name, verb.description, Group::gameplay, std::move(arguments));
        entry.echo_input = !verb.quiet;
        entry.run = [name = std::string(verb.name)](const Model &model, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0])) {
                    std::size_t start = 0;
                    while (start < text->size()) {
                        auto end = text->find(' ', start);
                        if (end == std::string::npos) end = text->size();
                        if (end > start) words.push_back(text->substr(start, end - start));
                        start = end + 1;
                    }
                }
            out(filmer::command(name, words, model.debug.game_ui_hidden));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
