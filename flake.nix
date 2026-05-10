{
  description = "baa - a simple C++20 arena allocator library";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      llvm = pkgs.llvmPackages_18;
    in
    {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "baa";
        version = "0.1.0";
        src = self;
        dontBuild = true;
        installPhase = ''
          mkdir -p $out/include
          cp -r include/baa $out/include/
        '';
        meta = {
          description = "A simple C++20 arena allocator library";
          homepage = "https://github.com/nova/arena";
        };
      };

      devShells.${system}.default = pkgs.mkShell.override { stdenv = llvm.stdenv; } {
        name = "baa-dev";
        packages = [
          llvm.clang
          llvm.clang-tools
          pkgs.cmake
          pkgs.ninja
          pkgs.pkg-config
          pkgs.gtest
          pkgs.gbenchmark
          pkgs.doxygen
        ];

        shellHook = ''
          echo "baa dev shell - clang $(clang --version | head -1)"
        '';
      };
    };
}
