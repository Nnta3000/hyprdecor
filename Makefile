PLUGIN_NAME = hyprdecor

all: release

release:
	cmake -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build

debug:
	cmake -B build -DCMAKE_BUILD_TYPE=Debug
	cmake --build build

clean:
	rm -rf build

install: release
	cmake --install build

release-patch:
	./scripts/release.sh patch

release-minor:
	./scripts/release.sh minor

release-major:
	./scripts/release.sh major

.PHONY: all release debug clean install release-patch release-minor release-major
