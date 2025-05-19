{
  godot_4,
  fetchFromGitHub,
  fetchpatch,
}: let
	overrides = {
		withX11 = false;
	};
in
(godot_4.override overrides).overrideAttrs (
  finalAttrs: prevAttrs: {
    version = "4.5-dev4";
	  commit = "209a446e3657e6fd736b9b7589b94cbdaad2d854";

    src = fetchFromGitHub {
      owner = "godotengine";
      repo = "godot";
      rev = "209a446e3657e6fd736b9b7589b94cbdaad2d854";
      hash = "sha256-EuddxyMXTjTNtfzbbBBwtrVF+0jZL7vKf5q6RJelkr0=";
    };

    patches = [
      (fetchpatch {
        url = "https://github.com/godotengine/godot/pull/106394.patch";
        hash = "sha256-C73SNrRGUjwKtn1q2BytuohsX5PznLteztv5OqJuqeQ=";
      })
    ];

    sconsFlags = prevAttrs.sconsFlags ++ ["builtin_freetype=false"];
  }
)
