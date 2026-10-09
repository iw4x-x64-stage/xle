# xle - Xbox Live social services server for IW4x.

`xle` serves the Xbox Live social services that IW4x connects to: the
social relationships (friends and followers), presence, multiplayer
activities and invites, privacy, and the real-time activity notifications.

The repository holds the following packages:

* [`xle`](xle/README.md): the server executable.
* [`libxle`](libxle/README.md): the services library.

Documentation: https://iw4x.io/projects/xle/doc/

## Usage

See the package `README.md` files listed above.

## Development

The development setup uses the standard `bdep`-based workflow and needs a
C++26 compiler (GCC 16 or later) and PostgreSQL. For example:

```
git clone https://github.com/iw4x-x64-stage/xle.git
cd xle

bdep init -C @gcc cc config.cxx=g++ config.cc.compiledb=./
bdep update
bdep test
```

The `config.cc.compiledb` value makes the build maintain
`compile_commands.json` in the repository root, which `.clangd` points
`clangd` to.

The PostgreSQL stores are tested only if a maintenance database is
specified with `config.libxle.test.pgsql` (see
`libxle/tests/build/root.build`), for example:

```
bdep test '!config.libxle.test.pgsql=postgres'
```

## Contributing

See https://github.com/iw4x/.github/blob/main/CONTRIBUTING.md

## License

xle is licensed under the GNU General Public License, version 3, subject
to the additional permissions described in version 1.1 of the IW4x
Linking Exception.

See LICENSE.md, LICENSE-EXCEPTION.md, and AUTHORS.
