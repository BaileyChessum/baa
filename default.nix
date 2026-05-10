{ pkgs ? import <nixpkgs> {} }:

pkgs.stdenv.mkDerivation {
  pname = "baa";
  version = "0.1.0";

  src = ./.;

  dontBuild = true;

  installPhase = ''
    mkdir -p $out/include
    cp -r include/baa $out/include/
  '';

  meta = with pkgs.lib; {
    description = "A simple C++20 arena allocator library";
    license = licenses.mit;
  };
}
