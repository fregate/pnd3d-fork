#include <spdlog/spdlog.h>

#include <raiiglfw/IODevice.h>
#include <raiiglfw/System.h>

int main(int argc, char ** argv)
{
	spdlog::info("Run main()");
	raiiglfw::System main(
		[](int c, const char * desc)
		{
			spdlog::error("Code: {}; Description: {}", c, desc);
		});

	const auto dev = raiiglfw::make_io_device(800, 600, "First");
	dev->set_key_callback([](int key, int scancode, int action, int mods) {
		spdlog::info("key pressed: {}, {}, {}, {}", key, scancode, action, mods);
	});
	dev->init();
	dev->run();

	spdlog::info("Exit main");
	return 0;
}
