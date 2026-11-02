# Contributing

Contributions are welcome. For anything beyond a small fix, please open an issue first to discuss it. To build and test, see [docs/BUILDING.md](docs/BUILDING.md).

- Keep the two layers apart: nothing game-specific (addresses, structures) in `src/core/`.
- Every hook must check the game's bytes before installing and fall back to the original behavior on failure.
- Never add files from the game (art, text, data) or derived from them to the repository.
- No AI-generated or AI-upscaled images (backgrounds, artwork, interface graphics): the project keeps the original art style, so such contributions will not be accepted.
- Run the offline tests before submitting.
- By contributing you agree that your contribution is licensed under the AGPL-3.0.
