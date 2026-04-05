// Test para verificar el hash del génesis
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <vector>
#include <hash.h>
#include <uint256.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>

using namespace std;

// Copia de la función CreateGenesisBlock
static CBlock CreateGenesisBlockTest(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << nBits << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

int main() {
    // Parámetros exactos del script Python
    const char* pszTimestamp = "The Times 03/Jan/2009 Chancellor on brink of second bailout for banks";
    std::string pubkey_hex = "04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f";
    
    // Convertir pubkey hex a bytes
    std::vector<unsigned char> pubkey_bytes;
    for (size_t i = 0; i < pubkey_hex.length(); i += 2) {
        pubkey_bytes.push_back(std::stoi(pubkey_hex.substr(i, 2), nullptr, 16));
    }
    
    CScript genesisOutputScript = CScript() << pubkey_bytes << OP_CHECKSIG;
    
    uint32_t nTime = 1775299639;
    uint32_t nNonce = 4104778;
    uint32_t nBits = 0x1e00ffff;
    int32_t nVersion = 1;
    CAmount genesisReward = 67 * COIN;
    
    CBlock genesis = CreateGenesisBlockTest(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
    
    uint256 hash = genesis.GetHash();
    uint256 merkle = genesis.hashMerkleRoot;
    
    cout << "=== RESULTADOS ===" << endl;
    cout << "Timestamp: " << nTime << endl;
    cout << "Nonce: " << nNonce << endl;
    cout << "nBits: 0x" << hex << nBits << dec << endl;
    cout << "Reward: " << genesisReward << " satoshis (" << (genesisReward / COIN) << " LDO)" << endl;
    cout << "Mensaje: " << pszTimestamp << endl;
    cout << endl;
    cout << "Block Hash: " << hash.GetHex() << endl;
    cout << "Block Hash (revertido): ";
    for (int i = 31; i >= 0; i--) {
        cout << setfill('0') << setw(2) << hex << (int)hash.begin()[i];
    }
    cout << dec << endl;
    cout << "Merkle Root: " << merkle.GetHex() << endl;
    cout << endl;
    cout << "=== PARA chainparams.cpp ===" << endl;
    cout << "genesis = CreateGenesisBlock(" << nTime << ", " << nNonce << ", 0x" << hex << nBits << dec << ", 1, 67 * COIN);" << endl;
    cout << "consensus.hashGenesisBlock = uint256{\"";
    for (int i = 31; i >= 0; i--) {
        cout << setfill('0') << setw(2) << hex << (int)hash.begin()[i];
    }
    cout << dec << "\"};" << endl;
    cout << "assert(genesis.hashMerkleRoot == uint256{\"" << merkle.GetHex() << "\"});" << endl;
    
    return 0;
}
