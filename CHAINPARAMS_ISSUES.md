# 🔍 Problemas Encontrados en chainparams.cpp

## ⚠️ CRÍTICOS (pueden causar errores)

### 1. BIP34Hash incorrecto en MAINNET
**Línea ~88:**
```cpp
consensus.BIP34Hash = uint256{"000000000000024b89b42a942fe0d9fea3bb44ab7bd1b19115dd6a759c0808b8"};
```
❌ Este es el hash de Bitcoin mainnet, NO de LambDanio.
✅ Debería ser: `000000d3cb4268a929297984e98dd993f3c852dfaca66e18b400510a0bb7a7c4`

### 2. BIP34Hash incorrecto en TESTNET
**Línea ~178:**
```cpp
consensus.BIP34Hash = uint256{"00000019ded61d7539c2113b67795786dcc2e94a55e02be6339c07a9ea8d6de6"};
```
❌ Hash antiguo, no coincide con el nuevo génesis.
✅ Debería ser: `000000d3cb4268a929297984e98dd993f3c852dfaca66e18b400510a0bb7a7c4`

### 3. chainTxData.nTime inconsistente
**Línea ~148 (MAINNET):**
```cpp
.nTime = 1775203195,
```
❌ Este timestamp es ANTERIOR al génesis (1775299639).
✅ Debería ser >= al timestamp del génesis.

### 4. m_assumed_blockchain_size = 856
**Línea ~120:**
```cpp
m_assumed_blockchain_size = 856;
```
❌ Esto asume 856GB de bloques, pero la cadena está vacía.
✅ Debería ser `0` para una cadena nueva.

## ⚡ RECOMENDADOS (pueden causar warnings)

### 5. defaultAssumeValid = null hash
```cpp
consensus.defaultAssumeValid = uint256{"0000000000000000000000000000000000000000000000000000000000000000"};
```
⚠️ Para una cadena nueva está OK, pero cuando tengas bloques, actualízalo al último bloque confiable.

### 6. nMinimumChainWork = 0
```cpp
consensus.nMinimumChainWork = uint256{"0000000000000000000000000000000000000000000000000000000000000000"};
```
⚠️ OK para cadena nueva, pero actualízalo cuando la cadena crezca.

## 📋 Heights (todos en 0 - OK para cadena nueva)

| Height | MAIN | TEST | TEST4 | SIGNET | REGTEST |
|--------|------|------|-------|--------|---------|
| BIP34  | 0    | 0    | 0     | 1      | 0       |
| BIP65  | 0    | 0    | 0     | 1      | 0       |
| BIP66  | 0    | 0    | 0     | 1      | 0       |
| CSV    | 0    | 0    | 0     | 1      | 0       |
| Segwit | 0    | 0    | 0     | 1      | 0       |

✅ Todos en 0 significa que las reglas activan desde el génesis - correcto para una cadena nueva.

## 🔧 TESTNET4 y SIGNET

- **TESTNET4**: Tiene su propio génesis con mensaje personalizado - INTENCIONAL, está bien.
- **SIGNET**: Usa génesis de Bitcoin Signet - probablemente INTENCIONAL para compatibilidad.
- **REGTEST**: Génesis propio con dificultad mínima - CORRECTO.

---
*Generado: 2026-04-04*
