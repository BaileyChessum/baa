# Compatibility shim for `nix-shell`. Delegates to the flake devShell.
# Requires nix with the `flakes` experimental feature enabled.
(builtins.getFlake (toString ./.)).devShells.${builtins.currentSystem}.default
