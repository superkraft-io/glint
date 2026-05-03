#pragma once

/**
 * glint_element_tree.hpp
 * Tree-linkage implementation for glint_element.
 *
 * Included by glint_element.hpp after the full class declaration.
 * This file will eventually own the out-of-line implementations of:
 *
 *   appendChild(node)         — addChild; sets mpG, mRoot, mParent, mId, finalizes the subtree
 *   removeChild(node)         — removes & destroys a specific child
 *   replaceChildren()         — clearChildren; destroys all children, resets scroll ptrs
 *   parentElement()           — returns mParent
 *   children                  — mChildren (vector<unique_ptr<glint_element>>)
 *   tagName() / typeName()    — element type string
 *   mId                       — stable uint64_t node ID (assigned by appendChild)
 *   mTag                      — optional int tag for GetNodeWithTag()
 *   callRootTreeChanged()     — defined in glint_document.hpp (needs full root definition)
 *
 * STATUS: STUB — implementation currently in glint_element.hpp.
 *         Migration pending (see web-refactor.md step 3).
 */
