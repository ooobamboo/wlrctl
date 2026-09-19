project(
	'wlrctl',
	'c',
	version: '0.2.2',
	license: 'MIT',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'werror=true',
	],
)

cc = meson.get_compiler('c')

datadir = get_option('datadir')
prefix = get_option('prefix')


add_project_arguments('-Wno-unused-parameter', language: 'c')
add_project_arguments('-Wno-missing-braces', language: 'c')

version = '"@0@"'.format(meson.project_version())
add_project_arguments('-DWLRCTL_VERSION=@0@'.format(version), language: 'c')

if cc.has_function('memfd_create')
  add_project_arguments('-DMEMFD_CREATE', language: 'c')
endif

xkbcommon = dependency('xkbcommon')
wayland_client = dependency('wayland-client')

subdir('protocol')

src_files = [
	'main.c',
	'keyboard.c',
	'pointer.c',
	'toplevel.c',
	'output.c',
	'util.c',
]

includes = include_directories('include')

executable(
	'wlrctl',
	files(src_files),
	dependencies: [
		client_protos,
		wayland_client,
		xkbcommon,
	],
	include_directories: [includes],
	install: true
)

scdoc = dependency('scdoc', native: true, required: get_option('man-pages'))
if scdoc.found()
	scdoc_cmd = find_program(scdoc.get_variable(pkgconfig: 'scdoc'), native: true)
	sh = find_program('sh', native: true)
	mandir = get_option('mandir')
	output = 'wlrctl.1'
	custom_target(
		output,
		input: 'wlrctl.1.scd',
		output: output,
		command: [sh, '-c', '@0@ < @INPUT@ > @1@'.format(scdoc_cmd.full_path(), output)],
		install: true,
		install_dir: '@0@/man1'.format(mandir)
	)
endif

if get_option('zsh-completions')
	zsh_completions = files('completions/zsh/_wlrctl')
	zsh_install_dir = join_paths(datadir, 'zsh', 'site-functions')
	install_data(zsh_completions, install_dir: zsh_install_dir)
endif


# vim: set ts=4 sw=4:
