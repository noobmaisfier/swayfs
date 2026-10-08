lib_sway_client = static_library(
	'sway-client',
	files(
		'pool-buffer.c',
	),
	dependencies: [
		cairo,
		fcft,
		fontconfig,
		pixman,
		wayland_client
	],
	link_with: [lib_sway_common],
	include_directories: sway_inc
)
