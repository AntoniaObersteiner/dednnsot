#include <argp.h>
#include <cstdlib>
#include <format>
#include <iostream>
#include <list>
#include <portaudio.h>
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

	class Args {
	public:
		bool print_text     { false };
		int  tick_ms        { 100 };
		int  training_level {   2 };
		int  line_length    {  25 };
		int  line_count     {   5 };
	};

	// pa_callback shared data
	float left_phase  {  0.0f };
	float right_phase {  0.0f };
	bool  playing	  { false };

	PaStream * pa_stream;

	Args args;

	Trainer (
		const Args & args
	) : args(args) {
		pa_setup();
	}

	virtual ~Trainer () {
		pa_call(" closing", Pa_CloseStream, pa_stream);
		pa_call(" terminating", Pa_Terminate);
	}

	size_t word_length () const {
		return std::rand() % 8 + 2;
	}

	std::vector<bool> morse_bits(char letter) {
		const std::string & code = morse_code.at(letter);
		std::vector<bool> result;
		for (const char l : code) {
			auto tail = [&] () {
			switch (l) {
			case '.': return std::list<bool>{1,    0};
			case '-': return std::list<bool>{1, 1, 0};
			case ' ': return std::list<bool>{0, 0, 0};
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
		return result;
	}

	std::string generate () const {
		const std::string used = chars.substr(0, args.training_level);
		std::string result = "";
		while (result.size() < args.line_length) {
			for (int i = 0; i < word_length() && result.size() < args.line_length; i++) {
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
				std::cerr << c;
				for (const bool bit : morse_bits(c)) {
					playing = bit;
					Pa_Sleep(args.tick_ms);
				}
			}
			std::cerr << std::endl;
		});
	}

	int train () {
		std::string output = generate();

		pa_call(" starting", Pa_StartStream, pa_stream);

		std::cout << "Type what you hear:" << std::endl;
		auto thread = play_async(output);
		std::string input;
		getline(std::cin, input);
		thread.join();
		std::cout << " input: '" <<  input << "'" << std::endl;
		std::cout << "output: '" << output << "'" << std::endl;

		pa_call(" stopping", Pa_StopStream, pa_stream);

		int errors = difference(input, output);
		std::cout << "Errors: " << errors << " / " << output.size() << std::endl;
		return errors;
	}

	void step_and_write_to_buffer (float * & out) {
		if (!playing) {
			left_phase = 0.0f;
			right_phase = 0.0f;
		} else {
			// simple sawtooth phaser between -1.0 and 1.0.
			left_phase += 0.01f;
			if (left_phase >= 1.0f) left_phase -= 2.0f;
			// right has higher pitch
			right_phase += 0.03f;
			if (right_phase >= 1.0f) right_phase -= 2.0f;
		}

		*out++ =  left_phase;
		*out++ = right_phase;
	}

	void pa_setup () {
		constexpr unsigned long SAMPLE_RATE = 44100;

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
			SAMPLE_RATE,
			256,
			pa_callback,
			this
		);
	}
};

const std::string Trainer::chars = "mkrs";

const std::unordered_map<char, std::string> Trainer::morse_code {
	{ ' ', " " },
	{ '=', "-...-  " },
	{ 'm', "--" },
	{ 'k', "-.." },
	{ 'r', ".-." },
	{ 's', "..." },
	{ 'v', "...-" }
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

	for (unsigned i = 0; i < frames_per_buffer; i++) {
		trainer->step_and_write_to_buffer(out);
	}
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
  {"tick",       't', "TICK",   0, "use this many ms as morse tick" },
  {"line-len",   'n', "LENGTH", 0, "length of lines" },
  {"line-count", 'c', "LINES",  0, "number of lines" },
  {"level",      'l', "LEVEL",  0, "Koch learning level (>= 2)" },
  { 0 }
};
static constexpr int positional_arg_count = 0;

/* Used by main to communicate with parse_opt. */
struct arguments {
  int tick_ms, line_len, line_count, level;
};

/* Parse a single option. */
static error_t parse_opt (int key, char *arg, struct argp_state *state) {
	/* Get the input argument from argp_parse, which we
	know is a pointer to our arguments structure. */
	Trainer::Args * args = reinterpret_cast<Trainer::Args*>(state->input);

	switch (key) {
	case 't': args->tick_ms        = atoi(arg); break;
	case 'n': args->line_length    = atoi(arg); break;
	case 'c': args->line_count     = atoi(arg); break;
	case 'l': args->training_level = atoi(arg); break;
	case 'p': args->print_text     = true;      break;

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

	int error = trainer.train();
}
