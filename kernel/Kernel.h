#pragma once
namespace kernel{

	class Kernel{
	public:
		void start();
	protected:
		void loop();
		void initPaging();   // frame allocator + identity map + enable CR0.PG
	};
};