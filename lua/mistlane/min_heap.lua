-- SPDX-License-Identifier: Apache-2.0
-- Derived from game/Util/MinHeap.lua; module-local export.
--- 最小堆 ---
local MinHeap = {}
MinHeap.__index = MinHeap


function MinHeap:new( capacity, comp, ignore )
	local o = {}
	setmetatable( o, self )

	assert( capacity > 0 and comp )
	o.capacity  = capacity --- 最大容量
	---------------------------------
	--- comp定义function comp(a, b)
	---------------------------------
	o.comp 		= comp     --- 关键字比较规则
	o.ignore 	= ignore or function(...) end  --- 忽略元素规则
	o.size 		= 0        --- 当前元素个数
	o.list  	= {}       --- 堆顶元素
	return o
end

--------------------------------------
--- 根据comp规则插入，保证堆性质
--------------------------------------
function MinHeap:push( node )
	if self.ignore( node ) then return end

	if self.size == self.capacity then
		self:pushHead( node )
	else
		self:pushTail( node )
	end
end

------------------------------------
--- 堆顶插入并重建堆，仅当堆满的时候
------------------------------------
function MinHeap:pushHead( node )
	assert( self.list[1] )

	--- 新元素比堆顶小则丢弃
	if self.comp( node, self.list[1] ) then return end

	--- 替换堆顶
	self.list[1] = node

	--- 重建堆
	self:buildUpToDown( 1 )
end

-------------------------------
--- 堆尾插入并重建堆
-------------------------------
function MinHeap:pushTail( node )
	--- 插入堆尾，大小增加
	--table.insert( self.list, node )
	self.size = self.size + 1
	self.list[self.size] = node

	--- 重建堆
	self:buildDownToUp( self.size )
end

local swap = function( list, idx1, idx2 )
	if idx1 == idx2 then return end

	local temp = list[idx1]
	list[idx1] = list[idx2]
	list[idx2] = temp
end

---------------------------------
--- 自顶向下建堆,保证堆顶元素最小
---------------------------------
function MinHeap:buildUpToDown( idx )
	local list = self.list
	local comp = self.comp
	local size = self.size

	--- 逐层检查将最小元素上移，直到最顶层
	while idx * 2 <= size do
		local min = idx --- 记录最小元素下标

		--- 比较左节点
		local left = idx * 2
		if not comp( list[idx], list[left] ) then
			min = left
		end

		--- 比较右节点
		local right = idx * 2 + 1
		if right <= size then --- 右节点存在
			if not comp( list[min], list[right] ) then
				min = right
			end
		end

		--- 本身为最小元素则跳出循环
		if idx == min then break end

		--- 最小元素上提，继续检测下一层
		swap( list, idx, min )
		idx = min
	end
end

---------------------------------
--- 自底向上建堆,保证堆顶元素最小
---------------------------------
function MinHeap:buildDownToUp( idx )
	local list = self.list
	local comp = self.comp

	--- 一直检查到idx节点不存在父节点
	while idx / 2 >= 1 do
		--- 父节点
		local parent = math.floor( idx / 2 )

		--- 比父节点大则返回
		if not comp( list[idx], list[parent] ) then break end

		--- 比父节点小则向堆顶方向移动
		swap( list, idx, parent )
		idx = parent
	end
end

----------------------------
--- 弹出最小元素
----------------------------
function MinHeap:pop()
	if self.size == 0 then return end

	local list = self.list
	local node = list[1]

	--- 堆底元素移到堆顶
	list[1] = list[self.size]

	list[self.size] = nil
	self.size = self.size - 1

	self:buildUpToDown( 1 )

	return node
end

----------------------------
--- 返回元素列表
----------------------------
function MinHeap:sort( data )
	if not data then return end

	for k, v in pairs( data ) do
		self:push( v )
	end

	local sorted = {}
	for i=1, self.size do
		local node = self:pop()
		assert( node )
		--table.insert( sorted, node )
		sorted[i] = node
	end

	return sorted
end
return MinHeap
