{
  description = "ghostmind_core — anonymous therapy on Logos: LEZ registry + encrypted conversations + private payments";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # What the installed delivery_module 0.4.0 actually is (one commit before v0.3.3, same code).
    delivery_module.url = "github:logos-co/logos-delivery-module/v0.3.3";
    # LEZ v0.3.1, what the public testnet runs since its ~Oct 13 2026 reset.
    logos-execution-zone.url = "github:logos-blockchain/logos-execution-zone?rev=1a003bfbfb89fcf987f62975cd08a401169e1b8c";
    # Its lock pins a rust-overlay whose "stable.latest" predates the Rust 1.98
    # that v0.3.1 needs, so follow a current one.
    rust-overlay.url = "github:oxalica/rust-overlay";
    logos-execution-zone.inputs.rust-overlay.follows = "rust-overlay";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      externalLibInputs = {
        wallet_ffi = {
          input = inputs.logos-execution-zone;
          packages.default = "wallet";
        };
      };
    };
}
