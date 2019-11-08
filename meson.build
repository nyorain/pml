project(
	'ml',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=0.48.0',
	default_options: [
		'c_std=c11',
		'warning_level=3',
		'werror=true',
	],
)

add_project_arguments(
	[
		'-Wno-unused-parameter',
		'-Wundef',
		# '-Wvla',
		'-Wno-unused-function',
		'-Wno-unused-const-variable',
	],
	language: 'c',
)


lib_ml = library('posix_mainloop', ['mainloop.c'],
	install: true)

posix_mainloop_dep = declare_dependency(
	include_directories: [include_directories('.')],
	link_with: lib_ml)

# example/testing executable
dep_pulse = dependency('libpulse')
dep_playerctl = dependency('playerctl')
executable('mlexample', 'example.c',
	dependencies: [dep_pulse, dep_playerctl, posix_mainloop_dep])

executable('mltest-nested', 'test-nested.c',
	dependencies: [posix_mainloop_dep])
