## Requirements

- [conan](https://conan.io/) package manager
    ```sh
    pip3 install --user "conan>=1.47.0,<2.0"
    ```

    make sure to set the default ABI to C++11 with:

    ```sh
    conan profile new default --detect  # Generates default profile detecting GCC and sets old ABI
    conan profile update settings.compiler.libcxx=libstdc++11 default  # Sets libcxx to C++11 ABI
    ```

- cmake v3.9.x
- clang-format >= v6.0.0

## Build

Run from within the root:

```sh
./build.py chimera
```

This will create all conan packages required by chimera, and its executables.

__Note:__ If `gcc` is available, it is used as the default compiler. In a system with `clang` only, then `clang` becomes the default compiler. In any case, you can check the available compilers/compiler versions by calling `./build.py --help`.

---


## Usage

Refer to [ChimeRA's artifact repository](https://github.com/LPD-EPFL/swarm-artifacts) for detailed instructions on how to run the experiments.


## Navigating the code


The repository also includes the following submodules:
- fusee: an implementation of the FUSEE KVS
- conn: provides interfaces to easily create and manage reliable connections over RDMA.
- ctrl: provides interfaces to manage RDMA devices and control blocks.
- memory: provides datastructures used by ctrl
- memstore: provides an interface to access memcached. This module is used by conn to simplify the coordination of clients and servers for the configuration of RDMA connections.
- special: provides cmd-lines tools
- swarm-kv: the original implementation of the SWARM-KV kvs
- extern: provides the ibverbs and memcached external librairies
- shared: provides various tools used by multiple submodules
- third-party: provides third-party tools (used by conn)

The dependencies are described in [targets.yaml](targets.yaml)

format.sh helps format the workspaceto meat clang standards.