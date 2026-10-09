# libxle - Xbox Live services library for IW4x projects and infrastructure.

The `libxle` C++ library implements the Xbox Live social services as the
IW4x game client uses them: the social relationships (friends and
followers), presence, multiplayer activities and invites, privacy, and
the real-time activity notifications.

## Usage

To start using `libxle` in your project, add the following `depends`
value to your `manifest`, adjusting the version constraint as appropriate:

```
depends: libxle ^0.1.0
```

Then import the library in your `buildfile`:

```
import libs = libxle%lib{xle}
```
