#include <argp.h>
#include <cstdlib>
#include <ctime>
#include <format>
#include <iostream>
#include <iomanip>
#include <list>
#include <portaudio.h>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

struct pairhash {
public:
	template <typename T, typename U>
	std::size_t operator()(const std::pair<T, U> &x) const {
		return std::hash<T>()(x.first) ^ std::hash<U>()(x.second);
	}
};

PaStreamCallback pa_callback;

template <typename F, typename... Args>
void pa_call(const std::string & message, F&& f, Args... args) {
	auto error = f(args...);
	if (error != paNoError) {
		printf("PortAudio error%s: %s\n", message.c_str(), Pa_GetErrorText(error));
		exit(1);
	}
}


class Trainer {
public:
	static const std::string chars;
	static const std::unordered_map<char, std::string> morse_code;
	static constexpr unsigned long frames_per_second = 44100;

	long unsigned frame = 0;

	class Args {
	public:
		bool print_text     { false };
		bool draw_code      { false };
		int  wpm            {  15 };
		int  training_level {   2 };
		int  line_length    {  25 };
		int  line_count     {   5 };
		const char * text { nullptr };
	};

	// pulse audio playback buffer shared data
	float left_phase  {  0.0f };
	float right_phase {  0.0f };
	bool playing      { false }; // whether the note is pulse-audio-currently playing

	std::queue<bool> playing_bits;
	std::counting_semaphore<10> full {  0 };
	std::counting_semaphore<10> free { 10 };

	PaStream * pa_stream;

	Args args;

	Trainer (
		const Args & args
	) : args(args) {
		std::srand(std::time(0));
		pa_setup();
	}

	virtual ~Trainer () {
		pa_call(" closing", Pa_CloseStream, pa_stream);
		pa_call(" terminating", Pa_Terminate);
	}

	// 1 word == 5 letters,
	// so with an average 6 ticks per letter,
	// 1 word == 50 ticks
	// with a typing speed v in words / minute (number V = v unitless), we get
	// v = V words / minute = V * 50 ticks / (60000 ms)
	// rearrange for one tick:
	// 1 tick = 60.000 ms / (V * 50)
	double  s_per_tick () const { return 60.0 / (50.0 * args.wpm); }
	double ms_per_tick () const { return ms_per_tick() * 1000.0; }

	std::string print_config () const {
		std::stringstream s;
		s << "frames_per_second: " << frames_per_second   << std::endl;
		s << "args.wpm: "          << args.wpm            << std::endl;
		s << "args.level: "        << args.training_level << std::endl;
		s << "args.line_length: "  << args.line_length    << std::endl;
		s << "args.line_count: "   << args.line_count     << std::endl;
		return s.str();
	}

	std::string print_time () const {
		std::time_t t = std::time(nullptr);
		const char * const fmt = "%Y-%m-%d_%H-%M-%S";
		std::stringstream s;
		s << std::put_time(std::localtime(&t), fmt);
		return s.str();
	}

	std::vector<bool> morse_bits(char letter) {
		const std::string & code = morse_code.at(letter);
		std::vector<bool> result;
		for (const char l : code) {
			auto tail = [&] () {
			switch (l) {
			case '.': return std::list<bool>{1,       0};
			case '-': return std::list<bool>{1, 1, 1, 0};
			case ' ': return std::list<bool>{0, 0};
			default: throw std::runtime_error(std::format("unknown morse letter '{}'!", l));
			}
			} ();
			#ifdef __cpp_lib_containers_ranges
				result.append_range(tail);
			#else
				result.insert(result.end(), tail.cbegin(), tail.cend());
			#endif
		}
		result.push_back(0);
		result.push_back(0);
		return result;
	}

	std::string generate () const {
		const std::string used = chars.substr(0, args.training_level);
		std::string result = "";
		while (result.size() < args.line_length) {
			int word_length = std::rand() % 8 + 2;
			if (args.line_length == result.size() + word_length + 1)
				word_length++;
			for (int i = 0; i < word_length && result.size() < args.line_length; i++) {
				char chosen = used[std::rand() % used.size()];
				result.push_back(chosen);
			}
			result += " ";
		}
		result += "=";

		return result;
	}

	int difference (
		const std::string & output,
		const std::string & input
	) const {
		std::unordered_map<std::pair<int, int>, int, pairhash> d;
		for (int o = 0; o < output.size() + 1; o++) {
			if (o == 0) {
				for (int i = 0; i < input.size() + 1; i++) {
					d[std::make_pair(0, i)] = i;
				}
				continue;
			}
			d[std::make_pair(o, 0)] = o;

			for (int i = 0; i < input.size() + 1; i++) {
				auto pos   = std::make_pair(o,	 i);
				auto left  = std::make_pair(o - 1, i);
				auto above = std::make_pair(o,	 i - 1);
				auto diag  = std::make_pair(o - 1, i - 1);
				auto consider = [&] (int v) {
					if (!d.contains(pos) || v < d[pos]) {
						d[pos] = v;
					}
				};
				if (input[i - 1] == output[o - 1]) {
					consider(d[diag]);
				}
				consider(d[left]  + 1);
				consider(d[above] + 1);
				consider(d[diag]  + 1);
			}
		}

		return d[std::make_pair(output.size(), input.size())];
	}

	std::thread play_async (const std::string & output) {
		return std::thread([&] () {
			for (const auto & c : output) {
				for (const bool bit : morse_bits(c)) {
					free.acquire();
					playing_bits.push(bit);
					full.release();
				}
				if (args.print_text) std::cerr << c;
			}
			if (args.print_text) std::cerr << std::endl;
		});
	}

	void play (const std::string & output) {
		play_async(output).join();
	}

	float train () {
		pa_call(" starting", Pa_StartStream, pa_stream);

		std::cout << print_config() << std::endl;

		if (!args.text) {
			std::cout << "Type what you hear after the 'vvv'!" << std::endl;
			std::cout << "Press enter after the '=' (eval at the end)." << std::endl;
			play("vvv");
		}

		int errors = 0;
		int symbols = 0;
		std::vector<std::string> outputs;
		std::vector<std::string>  inputs;
		std::vector<std::string>  starts;
		std::vector<std::string>   stops;

		for (int line_number = 0; line_number < args.line_count; line_number++) {
			const std::string & output = (
				args.text ? std::string{args.text} : generate()
			);
			auto thread = play_async(output);
			starts.push_back(print_time());
			std::string input;
			getline(std::cin, input);
			stops.push_back(print_time());
			thread.join();

			errors += difference(input, output);
			// for " =" at the end of each line
			symbols += output.size() - 2 * !(args.text);

			outputs.push_back(output);
			inputs .push_back( input);
		}

		for (int line_number = 0; line_number < args.line_count; line_number++) {
			std::cout << "output: '" << outputs[line_number] << "'" << std::endl;
			std::cout << " input: '" <<  inputs[line_number] << "' (error: "
				<< difference(inputs[line_number], outputs[line_number]) << ")"
				<< std::endl;
			std::cout << " start: '" <<  starts[line_number] << "'" << std::endl;
			std::cout << "  stop: '" <<   stops[line_number] << "'" << std::endl;
		}

		std::cout << "Errors: " << errors << " / " << symbols << std::endl;

		pa_call(" stopping", Pa_StopStream, pa_stream);

		return static_cast<float>(errors) / symbols;
	}

	void step_and_write_to_buffer (float * & out, const int frames_per_buffer) {
		const unsigned long frames_per_tick = frames_per_second * s_per_tick();

		long unsigned target_frame = frame + frames_per_buffer;
		long unsigned morse_bit_number = 0;

		for (; frame < target_frame; frame++) {
			if (frame % frames_per_tick == 0) {
				if (!playing_bits.empty()) {
					full.acquire();
					playing = playing_bits.front();
					playing_bits.pop();
					morse_bit_number++;
					free.release();
					if (args.draw_code)
						std::cout << (playing ? '#' : '_') << std::flush;
				} else {
					playing = false;
				}
			}

			// bool left = !(morse_bit_number % 2), right = playing;
			bool left = playing, right = playing;

			if (!left) {
				left_phase = 0.0f;
			} else {
				// simple sawtooth phaser between -1.0 and 1.0.
				left_phase += 0.01f;
				if (left_phase >= 1.0f) left_phase -= 2.0f;
			}

			if (!right) {
				right_phase = 0.0f;
			} else {
				// right has higher pitch
				right_phase += 0.03f;
				if (right_phase >= 1.0f) right_phase -= 2.0f;
			}

			*out++ =  left_phase;
			*out++ = right_phase;
		}
	}

	void pa_setup () {
		pa_call(" initializing", Pa_Initialize);

		// paFramesPerBufferUnspecified tells PortAudio to pick the best,
		constexpr unsigned long count_channels_input = 0;
		constexpr unsigned long count_channels_output = 2;
		constexpr unsigned long frames_per_buffer = 256;
		// Open an audio I/O stream
		pa_call(
			" opening IO stream",
			Pa_OpenDefaultStream,
			&pa_stream,
			count_channels_input,
			count_channels_output,
			paFloat32,
			frames_per_second,
			256,
			pa_callback,
			this
		);
	}
};

const std::string Trainer::chars = "mkrsuaptlowinjef0yvg5q9zh8b?4xcd67123";

const std::unordered_map<char, std::string> Trainer::morse_code {
	{ ' ', " " },
	{ 'a', ".-" },
	{ 'b', "-..." },
	{ 'c', "-.-." },
	{ 'd', "-.." },
	{ 'e', "." },
	{ 'f', "..-." },
	{ 'g', "--." },
	{ 'h', "...." },
	{ 'i', ".." },
	{ 'j', ".---" },
	{ 'k', "-.-" },
	{ 'l', ".-.." },
	{ 'm', "--" },
	{ 'n', "-." },
	{ 'o', "---" },
	{ 'p', ".--." },
	{ 'q', "--.-" },
	{ 'r', ".-." },
	{ 's', "..." },
	{ 't', "-" },
	{ 'u', "..-" },
	{ 'v', "...-" },
	{ 'w', ".--" },
	{ 'x', "-..-" },
	{ 'y', "-.--" },
	{ 'z', "--.." },
#if 1
	// normal numbers
	{ '0', "-----" },
	{ '1', ".----" },
	{ '2', "..---" },
	{ '3', "...--" },
	{ '4', "....-" },
	{ '5', "....." },
	{ '6', "-...." },
	{ '7', "--..." },
	{ '8', "---.." },
	{ '9', "----." },
#else
	{ '0', 	"-" },
	{ '1', 	".-" },
	{ '2', 	"..-" },
	{ '3', 	"...-" },
	{ '4', 	"....-" },
	{ '5', 	"." },
	{ '6', 	"-...." },
	{ '7', 	"-..." },
	{ '8', 	"-.." },
	{ '9', 	"-." },
#endif
	{ 'E', "........" },
	{ '&', ".-..." },
	{ '\'', ".----." },
	{ '@', ".--.-." },
	{ ')', "-.--.-" },
	{ '(', "-.--." },
	{ ':', "---..." },
	{ ',', "--..--" },
	{ '=', "-...-" },
	{ '!', "-.-.--" },
	{ '.', ".-.-.-" },
	{ '-', "-....-" },
	{ 'X', "-..-" },
	{ '%', "----- -..-. -----" },
	{ '+', ".-.-." },
	{ '"', ".-..-." },
	{ '?', "..--.." },
	{ '/', "-..-." }
};


// This routine will be called by the PortAudio engine when audio is needed.
int pa_callback(
	void const *  input_buffer,
	void	   * output_buffer,
	unsigned long frames_per_buffer,
	PaStreamCallbackTimeInfo const * time_info,
	PaStreamCallbackFlags status_flags,
	void * user_data
) {
	Trainer * trainer = reinterpret_cast<Trainer *>(user_data);
	float * out = reinterpret_cast<float *>(output_buffer);
	(void) input_buffer;

	trainer->step_and_write_to_buffer(out, frames_per_buffer);
	return 0;
}

const char *argp_program_version = "morse 0.1";
const char *argp_program_bug_address = "<antonia.obersteiner@gmail.com>";
static char doc[] = "Morse Code trainer according to Koch method.";
/* A description of the arguments we accept. */
static char args_doc[] = "ARG1 ARG2"; // TODO

/* The options we understand. */
static struct argp_option options[] = {
  {"print-text", 'p', 0,        0, "print the text that is morsed." },
  {"draw-code",  'd', 0,        0, "draw the morse code while playing" },
  {"wpm",        'w', "WPM",    0, "words per minute (uses 50 ticks per word)" },
  {"line-len",   'n', "LENGTH", 0, "length of lines" },
  {"line-count", 'c', "LINES",  0, "number of lines" },
  {"level",      'l', "LEVEL",  0, "Koch learning level (>= 2)" },
  {"text",       't', "TEXT",   0, "what text to use (instead of random text)" },
  { 0 }
};
static constexpr int positional_arg_count = 0;

/* Parse a single option. */
static error_t parse_opt (int key, char *arg, struct argp_state *state) {
	/* Get the input argument from argp_parse, which we
	know is a pointer to our arguments structure. */
	Trainer::Args * args = reinterpret_cast<Trainer::Args*>(state->input);

	switch (key) {
	case 'w': args->wpm            = atoi(arg); break;
	case 'n': args->line_length    = atoi(arg); break;
	case 'c': args->line_count     = atoi(arg); break;
	case 'l': args->training_level = atoi(arg); break;
	case 'p': args->print_text     = true;      break;
	case 'd': args->draw_code      = true;      break;
	case 't': args->text           = arg;       break;

	case ARGP_KEY_ARG:
		if (state->arg_num >= positional_arg_count)
			/* Too many arguments. */
			argp_usage (state);
		break;

	case ARGP_KEY_END:
		if (state->arg_num < positional_arg_count)
			/* Not enough arguments. */
			argp_usage (state);
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

/* Our argp parser. */
static struct argp argp = { options, parse_opt, args_doc, doc };

int main (int argc, char * argv []) {
	Trainer::Args args;
	argp_parse(&argp, argc, argv, 0, 0, &args);

	Trainer trainer { args };
	trainer.train();
}
