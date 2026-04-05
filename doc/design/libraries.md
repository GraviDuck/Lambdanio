# Libraries

| Name                     | Description |
|--------------------------|-------------|
| *liblambdanio_cli*         | RPC client functionality used by *lambdanio-cli* executable |
| *liblambdanio_common*      | Home for common functionality shared by different executables and libraries. Similar to *liblambdanio_util*, but higher-level (see [Dependencies](#dependencies)). |
| *liblambdanio_consensus*   | Consensus functionality used by *liblambdanio_node* and *liblambdanio_wallet*. |
| *liblambdanio_crypto*      | Hardware-optimized functions for data encryption, hashing, message authentication, and key derivation. |
| *liblambdanio_kernel*      | Consensus engine and support library used for validation by *liblambdanio_node*. |
| *liblambdanioqt*           | GUI functionality used by *lambdanio-qt* and *lambdanio-gui* executables. |
| *liblambdanio_ipc*         | IPC functionality used by *lambdanio-node* and *lambdanio-gui* executables to communicate when [`-DENABLE_IPC=ON`](multiprocess.md) is used. |
| *liblambdanio_node*        | P2P and RPC server functionality used by *lambdaniod* and *lambdanio-qt* executables. |
| *liblambdanio_util*        | Home for common functionality shared by different executables and libraries. Similar to *liblambdanio_common*, but lower-level (see [Dependencies](#dependencies)). |
| *liblambdanio_wallet*      | Wallet functionality used by *lambdaniod* and *lambdanio-wallet* executables. |
| *liblambdanio_wallet_tool* | Lower-level wallet functionality used by *lambdanio-wallet* executable. |
| *liblambdanio_zmq*         | [ZeroMQ](../zmq.md) functionality used by *lambdaniod* and *lambdanio-qt* executables. |

## Conventions

- Most libraries are internal libraries and have APIs which are completely unstable! There are few or no restrictions on backwards compatibility or rules about external dependencies. An exception is *liblambdanio_kernel*, which, at some future point, will have a documented external interface.

- Generally each library should have a corresponding source directory and namespace. Source code organization is a work in progress, so it is true that some namespaces are applied inconsistently, and if you look at [`add_library(lambdanio_* ...)`](../../src/CMakeLists.txt) lists you can see that many libraries pull in files from outside their source directory. But when working with libraries, it is good to follow a consistent pattern like:

  - *liblambdanio_node* code lives in `src/node/` in the `node::` namespace
  - *liblambdanio_wallet* code lives in `src/wallet/` in the `wallet::` namespace
  - *liblambdanio_ipc* code lives in `src/ipc/` in the `ipc::` namespace
  - *liblambdanio_util* code lives in `src/util/` in the `util::` namespace
  - *liblambdanio_consensus* code lives in `src/consensus/` in the `Consensus::` namespace

## Dependencies

- Libraries should minimize what other libraries they depend on, and only reference symbols following the arrows shown in the dependency graph below:

<table><tr><td>

```mermaid

%%{ init : { "flowchart" : { "curve" : "basis" }}}%%

graph TD;

lambdanio-cli[lambdanio-cli]-->liblambdanio_cli;

lambdaniod[lambdaniod]-->liblambdanio_node;
lambdaniod[lambdaniod]-->liblambdanio_wallet;

lambdanio-qt[lambdanio-qt]-->liblambdanio_node;
lambdanio-qt[lambdanio-qt]-->liblambdanioqt;
lambdanio-qt[lambdanio-qt]-->liblambdanio_wallet;

lambdanio-wallet[lambdanio-wallet]-->liblambdanio_wallet;
lambdanio-wallet[lambdanio-wallet]-->liblambdanio_wallet_tool;

liblambdanio_cli-->liblambdanio_util;
liblambdanio_cli-->liblambdanio_common;

liblambdanio_consensus-->liblambdanio_crypto;

liblambdanio_common-->liblambdanio_consensus;
liblambdanio_common-->liblambdanio_crypto;
liblambdanio_common-->liblambdanio_util;

liblambdanio_kernel-->liblambdanio_consensus;
liblambdanio_kernel-->liblambdanio_crypto;
liblambdanio_kernel-->liblambdanio_util;

liblambdanio_node-->liblambdanio_consensus;
liblambdanio_node-->liblambdanio_crypto;
liblambdanio_node-->liblambdanio_kernel;
liblambdanio_node-->liblambdanio_common;
liblambdanio_node-->liblambdanio_util;

liblambdanioqt-->liblambdanio_common;
liblambdanioqt-->liblambdanio_util;

liblambdanio_util-->liblambdanio_crypto;

liblambdanio_wallet-->liblambdanio_common;
liblambdanio_wallet-->liblambdanio_crypto;
liblambdanio_wallet-->liblambdanio_util;

liblambdanio_wallet_tool-->liblambdanio_wallet;
liblambdanio_wallet_tool-->liblambdanio_util;

classDef bold stroke-width:2px, font-weight:bold, font-size: smaller;
class lambdanio-qt,lambdaniod,lambdanio-cli,lambdanio-wallet bold
```
</td></tr><tr><td>

**Dependency graph**. Arrows show linker symbol dependencies. *Crypto* lib depends on nothing. *Util* lib is depended on by everything. *Kernel* lib depends only on consensus, crypto, and util.

</td></tr></table>

- The graph shows what _linker symbols_ (functions and variables) from each library other libraries can call and reference directly, but it is not a call graph. For example, there is no arrow connecting *liblambdanio_wallet* and *liblambdanio_node* libraries, because these libraries are intended to be modular and not depend on each other's internal implementation details. But wallet code is still able to call node code indirectly through the `interfaces::Chain` abstract class in [`interfaces/chain.h`](../../src/interfaces/chain.h) and node code calls wallet code through the `interfaces::ChainClient` and `interfaces::Chain::Notifications` abstract classes in the same file. In general, defining abstract classes in [`src/interfaces/`](../../src/interfaces/) can be a convenient way of avoiding unwanted direct dependencies or circular dependencies between libraries.

- *liblambdanio_crypto* should be a standalone dependency that any library can depend on, and it should not depend on any other libraries itself.

- *liblambdanio_consensus* should only depend on *liblambdanio_crypto*, and all other libraries besides *liblambdanio_crypto* should be allowed to depend on it.

- *liblambdanio_util* should be a standalone dependency that any library can depend on, and it should not depend on other libraries except *liblambdanio_crypto*. It provides basic utilities that fill in gaps in the C++ standard library and provide lightweight abstractions over platform-specific features. Since the util library is distributed with the kernel and is usable by kernel applications, it shouldn't contain functions that external code shouldn't call, like higher level code targeted at the node or wallet. (*liblambdanio_common* is a better place for higher level code, or code that is meant to be used by internal applications only.)

- *liblambdanio_common* is a home for miscellaneous shared code used by different Lambdanio Core applications. It should not depend on anything other than *liblambdanio_util*, *liblambdanio_consensus*, and *liblambdanio_crypto*.

- *liblambdanio_kernel* should only depend on *liblambdanio_util*, *liblambdanio_consensus*, and *liblambdanio_crypto*.

- The only thing that should depend on *liblambdanio_kernel* internally should be *liblambdanio_node*. GUI and wallet libraries *liblambdanioqt* and *liblambdanio_wallet* in particular should not depend on *liblambdanio_kernel* and the unneeded functionality it would pull in, like block validation. To the extent that GUI and wallet code need scripting and signing functionality, they should be able to get it from *liblambdanio_consensus*, *liblambdanio_common*, *liblambdanio_crypto*, and *liblambdanio_util*, instead of *liblambdanio_kernel*.

- GUI, node, and wallet code internal implementations should all be independent of each other, and the *liblambdanioqt*, *liblambdanio_node*, *liblambdanio_wallet* libraries should never reference each other's symbols. They should only call each other through [`src/interfaces/`](../../src/interfaces/) abstract interfaces.

## Work in progress

- Validation code is moving from *liblambdanio_node* to *liblambdanio_kernel* as part of [The liblambdaniokernel Project #27587](https://github.com/lambdanio/lambdanio/issues/27587)
